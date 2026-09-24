#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "sqlite_backend.h"
#include "ast.h"
#include "query_plan.h"
#include "sqlite3.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <time.h>
#include <signal.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define SQL_MAX_TEXT (1024u * 1024u)
#define SQL_MAX_COLUMNS 256u

typedef struct {
    char *name;
    int kind; /* 0 unknown, 1 int64, 2 float64, 3 UTF-8 text */
    size_t capacity, used;
    bool *valid;
    union { int64_t *i; double *d; char **s; } values;
} SqlColumn;
struct MilenaSqlConnection { sqlite3 *db; atomic_bool cancelled; uint64_t deadline; bool timed; bool allow_transaction_statement; bool allow_schema_pragma; volatile sig_atomic_t *signal_cancel; };
static volatile sig_atomic_t sql_signal_cancelled;
static void sql_signal_handler(int signal_number) { (void)signal_number; sql_signal_cancelled = 1; }

static uint64_t now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter, frequency;
    if (QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&frequency) &&
        frequency.QuadPart > 0)
        return (uint64_t)((long double)counter.QuadPart * 1000.0L /
                          (long double)frequency.QuadPart);
#else
    struct timespec monotonic;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0)
        return (uint64_t)monotonic.tv_sec * 1000u +
               (uint64_t)monotonic.tv_nsec / 1000000u;
#endif
    /* Portable last-resort fallback; UTC can jump, but is used only where a
     * monotonic OS clock is unavailable. */
    struct timespec t;
    if (timespec_get(&t, TIME_UTC) != TIME_UTC) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void err(MilenaError *e, MilenaStatus s, const char *m) {
    if (e) milena_error_set(e, s, 0, 0, 0, m);
}
static void rollback_error(MilenaSqlConnection *c) {
    if (c && c->db && !sqlite3_get_autocommit(c->db)) {
        c->allow_transaction_statement = true;
        (void)sqlite3_exec(c->db, "ROLLBACK", NULL, NULL, NULL);
        c->allow_transaction_statement = false;
    }
}
static int progress(void *p) {
    MilenaSqlConnection *c = p;
    return atomic_load_explicit(&c->cancelled, memory_order_relaxed) ||
           (c->signal_cancel && *c->signal_cancel) ||
           (c->timed && now_ms() >= c->deadline);
}
static int authorize(void *p, int action, const char *a, const char *b,
                     const char *d, const char *t) {
    MilenaSqlConnection *c = p;
    (void)a; (void)b; (void)d; (void)t;
    if (action == SQLITE_ATTACH || action == SQLITE_DETACH ||
        (action == SQLITE_PRAGMA &&
         (!c || !c->allow_schema_pragma || !a ||
          sqlite3_stricmp(a, "table_xinfo") != 0)) ||
        ((action == SQLITE_TRANSACTION || action == SQLITE_SAVEPOINT) &&
         (!c || !c->allow_transaction_statement)))
        return SQLITE_DENY;
    return SQLITE_OK;
}
static bool utf8_ok(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i++];
        if (c < 0x80) continue;
        unsigned k = c >= 0xc2 && c <= 0xdf ? 1u : c >= 0xe0 && c <= 0xef ? 2u :
                     c >= 0xf0 && c <= 0xf4 ? 3u : 99u;
        if (k == 99u || i + k > n) return false;
        unsigned char f = s[i];
        if ((c == 0xe0 && f < 0xa0) || (c == 0xed && f >= 0xa0) ||
            (c == 0xf0 && f < 0x90) || (c == 0xf4 && f >= 0x90)) return false;
        for (unsigned j = 0; j < k; ++j) if ((s[i++] & 0xc0u) != 0x80u) return false;
    }
    return true;
}
MilenaSqlLimits milena_sql_default_limits(void) {
    MilenaSqlLimits l = {MILENA_SQL_DEFAULT_MAX_ROWS, MILENA_SQL_DEFAULT_MAX_BYTES,
                         MILENA_SQL_DEFAULT_TIMEOUT_MS}; return l;
}
MilenaStatus milena_sql_open(MilenaSqlConnection **out, const char *path, MilenaError *e) {
    if (e) milena_error_clear(e);
    if (!out || !path || !*path) { err(e, MILENA_ERR_ARGUMENT, "Ruta SQLite inválida"); return MILENA_ERR_ARGUMENT; }
    *out = NULL; MilenaSqlConnection *c = calloc(1, sizeof(*c));
    if (!c) { err(e, MILENA_ERR_MEMORY, "Sin memoria para conexión SQLite"); return MILENA_ERR_MEMORY; }
    atomic_init(&c->cancelled, false);
    int rc = sqlite3_open_v2(path, &c->db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) { if(c->db) sqlite3_close_v2(c->db); free(c); err(e,MILENA_ERR_IO,"No se pudo abrir la base SQLite"); return MILENA_ERR_IO; }
    (void)sqlite3_busy_timeout(c->db,250);
    (void)sqlite3_db_config(c->db,SQLITE_DBCONFIG_DEFENSIVE,1,NULL);
    (void)sqlite3_db_config(c->db,SQLITE_DBCONFIG_TRUSTED_SCHEMA,0,NULL);
    sqlite3_set_authorizer(c->db,authorize,c); sqlite3_progress_handler(c->db,1000,progress,c);
    *out=c; return MILENA_OK;
}
void milena_sql_close(MilenaSqlConnection *c) {
    if (!c) return; if(c->db){ rollback_error(c); sqlite3_progress_handler(c->db,0,NULL,NULL); (void)sqlite3_close_v2(c->db); } free(c);
}
void milena_sql_cancel(MilenaSqlConnection *c) { if(c){ atomic_store(&c->cancelled,true); if(c->db) sqlite3_interrupt(c->db); } }
void milena_sql_reset_cancel(MilenaSqlConnection *c) { if(c) atomic_store(&c->cancelled,false); }
static MilenaStatus tx(MilenaSqlConnection *c,const char *q,bool begin,MilenaError *e) {
    if(!c||!c->db){err(e,MILENA_ERR_ARGUMENT,"Conexión SQLite inválida");return MILENA_ERR_ARGUMENT;}
    bool active=sqlite3_get_autocommit(c->db)==0;
    if(active==begin){err(e,MILENA_ERR_DATA,"Estado de transacción SQLite inválido");return MILENA_ERR_DATA;}
    c->allow_transaction_statement=true;
    int rc=sqlite3_exec(c->db,q,NULL,NULL,NULL);
    c->allow_transaction_statement=false;
    if(rc!=SQLITE_OK){err(e,MILENA_ERR_DATA,"No se pudo completar la transacción SQLite");return MILENA_ERR_DATA;} return MILENA_OK;
}
MilenaStatus milena_sql_begin(MilenaSqlConnection*c,MilenaError*e){return tx(c,"BEGIN IMMEDIATE",true,e);}
MilenaStatus milena_sql_commit(MilenaSqlConnection*c,MilenaError*e){return tx(c,"COMMIT",false,e);}
MilenaStatus milena_sql_rollback(MilenaSqlConnection*c,MilenaError*e){return tx(c,"ROLLBACK",false,e);}
static void col_free(SqlColumn *c) {
    free(c->name); free(c->valid);
    if(c->kind==1)free(c->values.i); else if(c->kind==2)free(c->values.d);
    else if(c->kind==3){for(size_t i=0;i<c->used;++i)free(c->values.s[i]);free(c->values.s);}
}
static void cols_free(SqlColumn *c,size_t n){if(c){for(size_t i=0;i<n;++i)col_free(&c[i]);free(c);}}
static MilenaStatus reserve(SqlColumn*c,size_t n,MilenaError*e){
    if(n<=c->capacity && (!c->kind || c->values.i))return MILENA_OK;
    size_t cap=c->capacity?c->capacity:16; while(cap<n){if(cap>SIZE_MAX/2){err(e,MILENA_ERR_OVERFLOW,"Desbordamiento de resultado SQLite");return MILENA_ERR_OVERFLOW;}cap*=2;}
    if(cap>MILENA_SQL_HARD_MAX_ROWS)cap=MILENA_SQL_HARD_MAX_ROWS;
    if(cap<n){err(e,MILENA_ERR_DATA,"Límite de filas SQLite excedido");return MILENA_ERR_DATA;}
    bool *v=calloc(cap,sizeof(bool)); if(!v){err(e,MILENA_ERR_MEMORY,"Sin memoria para resultado SQLite");return MILENA_ERR_MEMORY;}
    if(c->valid)memcpy(v,c->valid,c->used*sizeof(bool));
    void *p=NULL;
    if(c->kind==1){p=calloc(cap,sizeof(int64_t));if(p&&c->values.i)memcpy(p,c->values.i,c->used*sizeof(int64_t));}
    else if(c->kind==2){p=calloc(cap,sizeof(double));if(p&&c->values.d)memcpy(p,c->values.d,c->used*sizeof(double));}
    else if(c->kind==3){p=calloc(cap,sizeof(char*));if(p&&c->values.s)memcpy(p,c->values.s,c->used*sizeof(char*));}
    if(c->kind&& !p){free(v);err(e,MILENA_ERR_MEMORY,"Sin memoria para celdas SQLite");return MILENA_ERR_MEMORY;}
    free(c->valid); c->valid=v;
    if(c->kind==1){free(c->values.i);c->values.i=p;} else if(c->kind==2){free(c->values.d);c->values.d=p;} else if(c->kind==3){free(c->values.s);c->values.s=p;}
    c->capacity=cap; return MILENA_OK;
}
static int decl_kind(const char *d){
    if(!d)return 0; char x[32];size_t n=0;while(*d&&n+1<sizeof x)x[n++]=(char)toupper((unsigned char)*d++);x[n]=0;
    if(strstr(x,"INT")||strstr(x,"BOOL"))return 1;
    if(strstr(x,"REAL")||strstr(x,"FLOA")||strstr(x,"DOUB"))return 2;
    if(strstr(x,"TEXT")||strstr(x,"CHAR")||strstr(x,"CLOB"))return 3;return 0;
}
static int typed_decl_kind(ASTSqlType type) {
    switch (type) {
    case AST_SQL_TYPE_INTEGER:
    case AST_SQL_TYPE_BOOLEAN: return 1;
    case AST_SQL_TYPE_REAL: return 2;
    case AST_SQL_TYPE_TEXT: return 3;
    default: return 0;
    }
}
static MilenaStatus bind_params(sqlite3_stmt*s,const MilenaSqlValue*v,size_t n,MilenaError*e){
    if((size_t)sqlite3_bind_parameter_count(s)!=n){err(e,MILENA_ERR_ARGUMENT,"Cantidad de parámetros SQLite incorrecta");return MILENA_ERR_ARGUMENT;}
    for(size_t i=0;i<n;++i){int r=SQLITE_MISUSE;switch(v[i].type){
        case MILENA_SQL_NULL:r=sqlite3_bind_null(s,(int)i+1);break;
        case MILENA_SQL_INT64:r=sqlite3_bind_int64(s,(int)i+1,v[i].as.i64);break;
        case MILENA_SQL_FLOAT64:if(!isfinite(v[i].as.f64)){err(e,MILENA_ERR_ARGUMENT,"Parámetro real SQLite no finito");return MILENA_ERR_ARGUMENT;}r=sqlite3_bind_double(s,(int)i+1,v[i].as.f64);break;
        case MILENA_SQL_TEXT:if(!v[i].as.text.data||v[i].as.text.length>INT_MAX||!utf8_ok((const unsigned char*)v[i].as.text.data,v[i].as.text.length)){err(e,MILENA_ERR_ARGUMENT,"Parámetro de texto SQLite inválido");return MILENA_ERR_ARGUMENT;}r=sqlite3_bind_text64(s,(int)i+1,v[i].as.text.data,(sqlite3_uint64)v[i].as.text.length,SQLITE_TRANSIENT,SQLITE_UTF8);break;
        default:err(e,MILENA_ERR_TYPE,"Tipo de parámetro SQL no admitido");return MILENA_ERR_TYPE;}
        if(r!=SQLITE_OK){err(e,MILENA_ERR_DATA,"No se pudo enlazar un parámetro SQLite");return MILENA_ERR_DATA;}}
    return MILENA_OK;
}
static MilenaStatus make_table(MilenaTable*out,SqlColumn*c,size_t nc,size_t nr,MilenaError*e){
    MilenaTable t;milena_table_init(&t);MilenaStatus st=MILENA_OK;
    for(size_t i=0;i<nc&&st==MILENA_OK;++i){
        if(!c[i].kind){c[i].kind=3;st=reserve(&c[i],nr,e);if(st!=MILENA_OK)break;}
        if(c[i].kind==3)st=milena_table_add_string_column_copy(&t,c[i].name,(const char*const*)c[i].values.s,nr,c[i].valid,e);
        else {MilenaArray a={0};size_t shape[1]={nr};
            st=c[i].kind==1?milena_array_from_i64(&a,1,shape,c[i].values.i,e):milena_array_from_f64(&a,1,shape,c[i].values.d,e);
            if(st==MILENA_OK)st=milena_table_add_column_copy(&t,c[i].name,&a,c[i].valid,e);milena_array_release(&a);}
    }
    if(st==MILENA_OK){t.row_count=nr;milena_table_swap(out,&t);}milena_table_destroy(&t);return st;
}
static MilenaStatus execute_impl(MilenaSqlConnection*c,const char*sql,const MilenaSqlValue*p,size_t np,const MilenaSqlLimits*given,MilenaTable*out,MilenaError*e,bool readonly,const MilenaSqlTypedProjection*typed_projection,size_t typed_projection_count){
    if(e)milena_error_clear(e);if(!c||!c->db||!sql||!out||(np&&!p)||np>MILENA_SQL_MAX_PARAMETERS||(typed_projection_count&&!typed_projection)){err(e,MILENA_ERR_ARGUMENT,"Argumentos de consulta SQLite inválidos");return MILENA_ERR_ARGUMENT;}
    MilenaSqlLimits l=given?*given:milena_sql_default_limits();if(!l.max_rows)l.max_rows=MILENA_SQL_DEFAULT_MAX_ROWS;if(!l.max_bytes)l.max_bytes=MILENA_SQL_DEFAULT_MAX_BYTES;if(!l.timeout_ms)l.timeout_ms=MILENA_SQL_DEFAULT_TIMEOUT_MS;
    if(l.max_rows>MILENA_SQL_HARD_MAX_ROWS||l.max_bytes>MILENA_SQL_HARD_MAX_BYTES||l.timeout_ms>MILENA_SQL_HARD_TIMEOUT_MS){err(e,MILENA_ERR_ARGUMENT,"Límite SQLite fuera del máximo permitido");return MILENA_ERR_ARGUMENT;}
    size_t len=0;while(len<=SQL_MAX_TEXT&&sql[len])++len;if(!len||len>SQL_MAX_TEXT){rollback_error(c);err(e,MILENA_ERR_ARGUMENT,"Sentencia SQLite vacía o demasiado larga");return MILENA_ERR_ARGUMENT;}
    sqlite3_stmt*s=NULL;const char*tail=NULL;int rc=sqlite3_prepare_v3(c->db,sql,(int)len,0,&s,&tail);
    if(rc!=SQLITE_OK||!s){if(s)sqlite3_finalize(s);rollback_error(c);err(e,MILENA_ERR_DATA,"No se pudo preparar la consulta SQLite");return MILENA_ERR_DATA;}
    if (tail && *tail) {
        sqlite3_stmt *extra = NULL;
        const char *extra_tail = NULL;
        int tail_rc = sqlite3_prepare_v3(c->db, tail, -1, 0, &extra, &extra_tail);
        (void)extra_tail;
        if (tail_rc != SQLITE_OK || extra) {
            if (extra) sqlite3_finalize(extra);
            sqlite3_finalize(s); rollback_error(c);
            err(e,MILENA_ERR_ARGUMENT,"Solo se permite una sentencia SQLite por operación");
            return MILENA_ERR_ARGUMENT;
        }
    }
    if(readonly&&!sqlite3_stmt_readonly(s)){sqlite3_finalize(s);rollback_error(c);err(e,MILENA_ERR_ARGUMENT,"La consulta Milena solo admite lectura");return MILENA_ERR_ARGUMENT;}
    if((size_t)sqlite3_bind_parameter_count(s)!=np){sqlite3_finalize(s);rollback_error(c);err(e,MILENA_ERR_ARGUMENT,"Cantidad de parámetros SQLite incorrecta");return MILENA_ERR_ARGUMENT;}
    if(atomic_load(&c->cancelled)){sqlite3_finalize(s);rollback_error(c);err(e,MILENA_ERR_UNSUPPORTED,"Consulta SQLite cancelada");return MILENA_ERR_UNSUPPORTED;}
    MilenaStatus st=bind_params(s,p,np,e);if(st!=MILENA_OK){sqlite3_finalize(s);rollback_error(c);return st;}
    c->deadline=now_ms()+l.timeout_ms;c->timed=true;size_t nc=(size_t)sqlite3_column_count(s);
    if(nc>SQL_MAX_COLUMNS){c->timed=false;sqlite3_finalize(s);rollback_error(c);err(e,MILENA_ERR_DATA,"Límite de columnas SQLite excedido");return MILENA_ERR_DATA;}
    if(typed_projection&&typed_projection_count!=nc){c->timed=false;sqlite3_finalize(s);rollback_error(c);err(e,MILENA_ERR_TYPE,"La cantidad de columnas SQLite no coincide con la proyección tipada");return MILENA_ERR_TYPE;}
    SqlColumn*cols=nc?calloc(nc,sizeof(*cols)):NULL;if(nc&&!cols){c->timed=false;sqlite3_finalize(s);rollback_error(c);err(e,MILENA_ERR_MEMORY,"Sin memoria para columnas SQLite");return MILENA_ERR_MEMORY;}
    size_t bytes=0,nr=0;for(size_t i=0;i<nc;++i){const char*name=sqlite3_column_name(s,(int)i);cols[i].name=milena_strdup(name?name:"columna");cols[i].kind=typed_projection?typed_decl_kind(typed_projection[i].type):decl_kind(sqlite3_column_decltype(s,(int)i));if(!cols[i].name){st=MILENA_ERR_MEMORY;err(e,st,"Sin memoria para nombre SQLite");break;}if(typed_projection&&!cols[i].kind){st=MILENA_ERR_TYPE;err(e,st,"Tipo de proyección SQL tipada no admitido");break;}size_t z=strlen(cols[i].name)+1;if(z>l.max_bytes-bytes){st=MILENA_ERR_DATA;err(e,st,"Límite de bytes SQLite excedido");break;}bytes+=z;}
    while(st==MILENA_OK&&(rc=sqlite3_step(s))==SQLITE_ROW){
        if(nr>=l.max_rows){st=MILENA_ERR_DATA;err(e,st,"Límite de filas SQLite excedido");break;}
        size_t addrow=0;
        for(size_t i=0;i<nc;++i){int t=sqlite3_column_type(s,(int)i);size_t z=(t==SQLITE_TEXT||t==SQLITE_BLOB)?(size_t)sqlite3_column_bytes(s,(int)i):8u;if(z>l.max_bytes-bytes-addrow){st=MILENA_ERR_DATA;err(e,st,"Límite de bytes SQLite excedido");break;}addrow+=z;if(t==SQLITE_NULL)continue;
            if(!cols[i].kind)cols[i].kind=t==SQLITE_INTEGER?1:t==SQLITE_FLOAT?2:t==SQLITE_TEXT?3:0;
            if(!cols[i].kind|| (cols[i].kind==1&&t!=SQLITE_INTEGER)||(cols[i].kind==2&&t!=SQLITE_FLOAT&&t!=SQLITE_INTEGER)||(cols[i].kind==3&&t!=SQLITE_TEXT)){st=MILENA_ERR_TYPE;err(e,st,"Tipo SQLite no representable en MilenaTable");break;}
            if(cols[i].kind==2&&t==SQLITE_INTEGER){int64_t x=sqlite3_column_int64(s,(int)i);if(x < -INT64_C(9007199254740992)||x>INT64_C(9007199254740992)){st=MILENA_ERR_TYPE;err(e,st,"Conversión int64 a real pierde precisión");break;}}
            st=reserve(&cols[i],nr+1,e);if(st!=MILENA_OK)break;
        }
        if(st!=MILENA_OK)break;
        for(size_t i=0;i<nc;++i){SqlColumn*q=&cols[i];int t=sqlite3_column_type(s,(int)i);st=reserve(q,nr+1,e);if(st!=MILENA_OK)break;
            if(t==SQLITE_NULL)q->valid[nr]=false;
            else if(q->kind==1){q->values.i[nr]=sqlite3_column_int64(s,(int)i);q->valid[nr]=true;}
            else if(q->kind==2){q->values.d[nr]=sqlite3_column_double(s,(int)i);q->valid[nr]=true;}
            else {const unsigned char*x=sqlite3_column_text(s,(int)i);int n=sqlite3_column_bytes(s,(int)i);if(n<0||(n&&(!x||memchr(x,0,(size_t)n)))||!utf8_ok(x,(size_t)n)){st=MILENA_ERR_DATA;err(e,st,"Texto SQLite no es UTF-8 representable");break;}q->values.s[nr]=malloc((size_t)n+1);if(!q->values.s[nr]){st=MILENA_ERR_MEMORY;err(e,st,"Sin memoria para texto SQLite");break;}if(n)memcpy(q->values.s[nr],x,(size_t)n);q->values.s[nr][n]=0;q->valid[nr]=true;}
            q->used=nr+1;
        }
        if(st!=MILENA_OK)break;bytes+=addrow;++nr;
    }
    c->timed=false;if(st==MILENA_OK&&rc!=SQLITE_DONE){st=rc==SQLITE_INTERRUPT?MILENA_ERR_UNSUPPORTED:MILENA_ERR_DATA;err(e,st,rc==SQLITE_INTERRUPT?((atomic_load(&c->cancelled)||(c->signal_cancel&&*c->signal_cancel))?"Consulta SQLite cancelada":"Tiempo de consulta SQLite excedido"):"Falló la consulta SQLite");}
    if(st==MILENA_OK)st=make_table(out,cols,nc,nr,e);if(st!=MILENA_OK)rollback_error(c);cols_free(cols,nc);sqlite3_finalize(s);return st;
}
MilenaStatus milena_sql_execute(MilenaSqlConnection*c,const char*s,const MilenaSqlValue*p,size_t n,const MilenaSqlLimits*l,MilenaTable*t,MilenaError*e){return execute_impl(c,s,p,n,l,t,e,false,NULL,0);}
MilenaStatus milena_sql_query(MilenaSqlConnection*c,const char*s,const MilenaSqlValue*p,size_t n,const MilenaSqlLimits*l,MilenaTable*t,MilenaError*e){return execute_impl(c,s,p,n,l,t,e,true,NULL,0);}

static void print_table(FILE*out,const MilenaTable*t){
    fprintf(out,"Tabla Milena: %zu filas, %zu columnas\n",t->row_count,t->column_count);
    for(size_t c=0;c<t->column_count;++c){if(c)fputc('\t',out);milena_json_write_string(out,t->columns[c].name);}fputc('\n',out);
    for(size_t r=0;r<t->row_count;++r){for(size_t c=0;c<t->column_count;++c){if(c)fputc('\t',out);if(milena_table_is_null(t,c,r)){fputs("nulo",out);continue;}const MilenaTableColumn*col=&t->columns[c];if(col->type==MILENA_COLUMN_STRING){const char*s=NULL;MilenaError e;if(milena_table_get_string(t,c,r,&s,&e)==MILENA_OK)milena_json_write_string(out,s?s:"");}else{const void*p=NULL;MilenaError e;if(milena_table_get_array_value(t,c,r,&p,&e)==MILENA_OK){if(col->values.dtype==MILENA_DTYPE_INT64)fprintf(out,"%" PRId64,*(const int64_t*)p);else fprintf(out,"%.17g",*(const double*)p);}}}fputc('\n',out);}
}
static void plan_value(const MilenaSqlPlanParameter *p, MilenaSqlValue *v) {
    memset(v, 0, sizeof(*v));
    switch (p->kind) {
    case MILENA_SQL_PLAN_NULL: v->type = MILENA_SQL_NULL; break;
    case MILENA_SQL_PLAN_INT64: v->type = MILENA_SQL_INT64; v->as.i64 = p->value.i64; break;
    case MILENA_SQL_PLAN_FLOAT64: v->type = MILENA_SQL_FLOAT64; v->as.f64 = p->value.f64; break;
    case MILENA_SQL_PLAN_TEXT: v->type = MILENA_SQL_TEXT; v->as.text.data = p->value.text.data; v->as.text.length = p->value.text.length; break;
    }
}

static bool type_contains_ascii(const char *type_name, const char *needle) {
    if (!type_name || !needle || !needle[0]) return false;
    for (const char *start = type_name; *start; ++start) {
        const char *left = start;
        const char *right = needle;
        while (*left && *right &&
               toupper((unsigned char)*left) == toupper((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (!*right) return true;
    }
    return false;
}

/* The typed contract uses SQLite's declared storage family, not just whatever
 * dynamic type happens to occur in the first result row. BOOLEAN is represented
 * as an integer-domain column and is value-checked when selected. */
static bool sqlite_decl_matches_milena_type(const char *declared,
                                            ASTSqlType expected) {
    bool integer = type_contains_ascii(declared, "INT");
    bool text = type_contains_ascii(declared, "CHAR") ||
                type_contains_ascii(declared, "CLOB") ||
                type_contains_ascii(declared, "TEXT");
    bool blob = !declared || !declared[0] ||
                type_contains_ascii(declared, "BLOB");
    bool real = type_contains_ascii(declared, "REAL") ||
                type_contains_ascii(declared, "FLOA") ||
                type_contains_ascii(declared, "DOUB");
    if (integer)
        return expected == AST_SQL_TYPE_INTEGER ||
               expected == AST_SQL_TYPE_BOOLEAN;
    if (text) return expected == AST_SQL_TYPE_TEXT;
    if (blob) return false;
    if (real) return expected == AST_SQL_TYPE_REAL;
    /* SQLite assigns NUMERIC affinity to BOOLEAN/BOOL and other numeric
     * declarations. The BOOLEAN contract additionally checks every value. */
    return expected == AST_SQL_TYPE_BOOLEAN;
}

static MilenaStatus lookup_sqlite_object(MilenaSqlConnection *connection,
                                         const char *database,
                                         const char *table_name,
                                         bool *found, bool *is_table,
                                         MilenaError *error) {
    char sql[128];
    int written = snprintf(sql, sizeof(sql),
        "SELECT type FROM %s.sqlite_schema WHERE name=?1 COLLATE BINARY",
        database);
    if (written < 0 || (size_t)written >= sizeof(sql)) {
        err(error, MILENA_ERR_INTERNAL, "No se pudo preparar la inspección del esquema SQLite");
        return MILENA_ERR_INTERNAL;
    }
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(connection->db, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK || !statement) {
        if (statement) sqlite3_finalize(statement);
        err(error, MILENA_ERR_DATA, "No se pudo inspeccionar el esquema físico de SQLite");
        return MILENA_ERR_DATA;
    }
    rc = sqlite3_bind_text(statement, 1, table_name, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(statement);
        err(error, MILENA_ERR_DATA, "No se pudo enlazar el nombre de tabla SQLite");
        return MILENA_ERR_DATA;
    }
    *found = false;
    *is_table = false;
    rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) {
        const unsigned char *kind = sqlite3_column_text(statement, 0);
        *found = true;
        *is_table = kind && strcmp((const char *)kind, "table") == 0;
        rc = sqlite3_step(statement);
    }
    int finalize_rc = sqlite3_finalize(statement);
    if ((rc != SQLITE_DONE && rc != SQLITE_ROW) || finalize_rc != SQLITE_OK) {
        err(error, MILENA_ERR_DATA, "Falló la inspección del esquema físico de SQLite");
        return MILENA_ERR_DATA;
    }
    return MILENA_OK;
}

static MilenaStatus validate_sqlite_schema(MilenaSqlConnection *connection,
                                           const ASTNode *schema,
                                           MilenaError *error) {
    bool main_found = false, main_is_table = false;
    MilenaStatus status = lookup_sqlite_object(connection, "main", schema->value,
        &main_found, &main_is_table, error);
    if (status != MILENA_OK) return status;
    if (!main_found || !main_is_table) {
        err(error, MILENA_ERR_TYPE,
            "La tabla tipada debe existir como tabla física en SQLite main");
        return MILENA_ERR_TYPE;
    }
    bool temp_found = false, temp_is_table = false;
    status = lookup_sqlite_object(connection, "temp", schema->value,
        &temp_found, &temp_is_table, error);
    (void)temp_is_table;
    if (status != MILENA_OK) return status;
    if (temp_found) {
        err(error, MILENA_ERR_TYPE,
            "Una tabla temporal SQLite no puede ocultar la tabla tipada declarada");
        return MILENA_ERR_TYPE;
    }

    static const char prefix[] = "PRAGMA main.table_xinfo(\"";
    static const char suffix[] = "\")";
    size_t name_length = strlen(schema->value);
    if (name_length > SQL_MAX_TEXT - sizeof(prefix) - sizeof(suffix)) {
        err(error, MILENA_ERR_ARGUMENT, "Nombre de tabla SQLite demasiado largo");
        return MILENA_ERR_ARGUMENT;
    }
    size_t query_size = sizeof(prefix) - 1u + name_length + sizeof(suffix);
    char *query = malloc(query_size);
    if (!query) {
        err(error, MILENA_ERR_MEMORY, "Sin memoria para inspeccionar el esquema SQLite");
        return MILENA_ERR_MEMORY;
    }
    (void)snprintf(query, query_size, "%s%s%s", prefix, schema->value, suffix);
    sqlite3_stmt *statement = NULL;
    connection->allow_schema_pragma = true;
    int rc = sqlite3_prepare_v2(connection->db, query, (int)(query_size - 1u),
                                &statement, NULL);
    free(query);
    if (rc != SQLITE_OK || !statement) {
        if (statement) sqlite3_finalize(statement);
        connection->allow_schema_pragma = false;
        err(error, MILENA_ERR_DATA, "No se pudo leer el esquema físico de la tabla SQLite");
        return MILENA_ERR_DATA;
    }

    size_t column_index = 0;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char *physical_name = sqlite3_column_text(statement, 1);
        const unsigned char *physical_type = sqlite3_column_text(statement, 2);
        int hidden = sqlite3_column_int(statement, 6);
        if (column_index >= schema->child_count || !physical_name || hidden != 0) {
            status = MILENA_ERR_TYPE;
            break;
        }
        const ASTNode *declared = schema->children[column_index];
        if (strcmp((const char *)physical_name, declared->value) != 0 ||
            !sqlite_decl_matches_milena_type(
                physical_type ? (const char *)physical_type : "",
                declared->sql_type)) {
            status = MILENA_ERR_TYPE;
            break;
        }
        ++column_index;
    }
    connection->allow_schema_pragma = false;
    int finalize_rc = sqlite3_finalize(statement);
    if (status == MILENA_OK && rc != SQLITE_DONE) {
        err(error, MILENA_ERR_DATA, "Falló la lectura del esquema físico de la tabla SQLite");
        status = MILENA_ERR_DATA;
    } else if (status == MILENA_OK && finalize_rc != SQLITE_OK) {
        err(error, MILENA_ERR_DATA, "No se pudo cerrar la inspección del esquema SQLite");
        status = MILENA_ERR_DATA;
    } else if (status == MILENA_OK && column_index != schema->child_count) {
        status = MILENA_ERR_TYPE;
    }
    if (status == MILENA_ERR_TYPE)
        err(error, status,
            "El esquema físico de SQLite no coincide en columnas u orden/tipos con el esquema tipado");
    return status;
}

static MilenaStatus validate_typed_schemas(MilenaSqlConnection *connection,
                                           const MilenaSqlExecutionPlan *plan,
                                           MilenaError *error) {
    for (size_t i = 0; i < plan->operation_count; ++i) {
        const MilenaSqlPlanOperation *operation = &plan->operations[i];
        if (operation->kind != MILENA_SQL_PLAN_TYPED_SELECT &&
            operation->kind != MILENA_SQL_PLAN_TYPED_INSERT &&
            operation->kind != MILENA_SQL_PLAN_TYPED_UPDATE)
            continue;
        bool already_checked = false;
        for (size_t prior = 0; prior < i; ++prior) {
            const MilenaSqlPlanOperation *previous = &plan->operations[prior];
            if ((previous->kind == MILENA_SQL_PLAN_TYPED_SELECT ||
                 previous->kind == MILENA_SQL_PLAN_TYPED_INSERT ||
                 previous->kind == MILENA_SQL_PLAN_TYPED_UPDATE) &&
                previous->typed_schema == operation->typed_schema) {
                already_checked = true;
                break;
            }
        }
        if (!already_checked) {
            MilenaStatus status = validate_sqlite_schema(connection,
                operation->typed_schema, error);
            if (status != MILENA_OK) return status;
        }
    }
    return MILENA_OK;
}

static MilenaStatus validate_typed_select_result(
        const MilenaSqlPlanOperation *operation, const MilenaTable *result,
        MilenaError *error) {
    if (!operation || !result || result->column_count != operation->projection_count) {
        err(error, MILENA_ERR_TYPE,
            "El resultado SELECT no coincide con la proyección tipada declarada");
        return MILENA_ERR_TYPE;
    }
    for (size_t column_index = 0; column_index < operation->projection_count;
         ++column_index) {
        const MilenaSqlTypedProjection *projection =
            &operation->projections[column_index];
        const MilenaTableColumn *column = &result->columns[column_index];
        bool matches = column->name && projection->name &&
                       strcmp(column->name, projection->name) == 0;
        switch (projection->type) {
        case AST_SQL_TYPE_INTEGER:
            matches = matches && column->type == MILENA_COLUMN_ARRAY &&
                      column->values.dtype == MILENA_DTYPE_INT64;
            break;
        case AST_SQL_TYPE_REAL:
            matches = matches && column->type == MILENA_COLUMN_ARRAY &&
                      column->values.dtype == MILENA_DTYPE_FLOAT64;
            break;
        case AST_SQL_TYPE_TEXT:
            matches = matches && column->type == MILENA_COLUMN_STRING;
            break;
        case AST_SQL_TYPE_BOOLEAN:
            matches = matches && column->type == MILENA_COLUMN_ARRAY &&
                      column->values.dtype == MILENA_DTYPE_INT64;
            break;
        default:
            matches = false;
            break;
        }
        if (!matches) {
            err(error, MILENA_ERR_TYPE,
                "El resultado SELECT no coincide con la proyección tipada declarada");
            return MILENA_ERR_TYPE;
        }
        if (projection->type == AST_SQL_TYPE_BOOLEAN) {
            for (size_t row = 0; row < result->row_count; ++row) {
                if (milena_table_is_null(result, column_index, row)) continue;
                const void *cell = NULL;
                if (milena_table_get_array_value(result, column_index, row,
                                                 &cell, error) != MILENA_OK ||
                    !cell || (*(const int64_t *)cell != 0 &&
                              *(const int64_t *)cell != 1)) {
                    err(error, MILENA_ERR_TYPE,
                        "El resultado booleano SQLite solo admite 0, 1 o nulo");
                    return MILENA_ERR_TYPE;
                }
            }
        }
    }
    return MILENA_OK;
}

MilenaStatus milena_sql_run_plan(const MilenaSqlExecutionPlan *plan, FILE *out,
                                 MilenaError *e) {
    MilenaStatus st = milena_sql_execution_plan_validate(plan, e);
    if (st != MILENA_OK) return st;
    sql_signal_cancelled = 0;
    void (*previous_sigint)(int) = signal(SIGINT, sql_signal_handler);
    MilenaSqlConnection *c = NULL;
    st = milena_sql_open(&c, plan->connection_path, e);
    if (st != MILENA_OK) {
        if (previous_sigint != SIG_ERR) (void)signal(SIGINT, previous_sigint);
        return st;
    }
    c->signal_cancel = &sql_signal_cancelled;
    st = validate_typed_schemas(c, plan, e);
    bool active = false;
    MilenaSqlLimits limits = {plan->max_rows, plan->max_bytes, plan->timeout_ms};
    FILE *dest = out ? out : stdout;
    for (size_t i = 0; i < plan->operation_count && st == MILENA_OK; ++i) {
        const MilenaSqlPlanOperation *op = &plan->operations[i];
        if (op->kind == MILENA_SQL_PLAN_SCHEMA) {
            /* In-block schemas are validation metadata, not DDL. */
            continue;
        } else if (op->kind == MILENA_SQL_PLAN_BEGIN) {
            st = milena_sql_begin(c, e); if (st == MILENA_OK) active = true;
        } else if (op->kind == MILENA_SQL_PLAN_COMMIT) {
            st = milena_sql_commit(c, e); if (st == MILENA_OK) active = false;
        } else if (op->kind == MILENA_SQL_PLAN_ROLLBACK) {
            st = milena_sql_rollback(c, e); if (st == MILENA_OK) active = false;
        } else {
            if (op->kind == MILENA_SQL_PLAN_TYPED_SELECT ||
                op->kind == MILENA_SQL_PLAN_TYPED_INSERT ||
                op->kind == MILENA_SQL_PLAN_TYPED_UPDATE) {
                /* Raw SQL is an explicit separate surface and may change the
                 * schema after the initial no-side-effect preflight. Recheck
                 * immediately before every typed operation as well. */
                st = validate_sqlite_schema(c, op->typed_schema, e);
                if (st != MILENA_OK) break;
            }
            MilenaSqlValue *values = op->parameter_count ? calloc(op->parameter_count, sizeof(*values)) : NULL;
            if (op->parameter_count && !values) { err(e, MILENA_ERR_MEMORY, "Sin memoria para parámetros SQL"); st = MILENA_ERR_MEMORY; break; }
            for (size_t j = 0; j < op->parameter_count; ++j) plan_value(&op->parameters[j], &values[j]);
            MilenaTable result; milena_table_init(&result);
            if (op->kind == MILENA_SQL_PLAN_TYPED_SELECT)
                st = execute_impl(c, op->statement, values, op->parameter_count,
                    &limits, &result, e, true, op->projections,
                    op->projection_count);
            else if (op->kind == MILENA_SQL_PLAN_QUERY)
                st = milena_sql_query(c, op->statement, values,
                                      op->parameter_count, &limits, &result, e);
            else
                st = milena_sql_execute(c, op->statement, values,
                                        op->parameter_count, &limits, &result, e);
            if (st == MILENA_OK && op->kind == MILENA_SQL_PLAN_TYPED_SELECT)
                st = validate_typed_select_result(op, &result, e);
            if (st == MILENA_OK &&
                (op->kind == MILENA_SQL_PLAN_QUERY ||
                 op->kind == MILENA_SQL_PLAN_TYPED_SELECT))
                print_table(dest, &result);
            if (st != MILENA_OK) active = false;
            milena_table_destroy(&result); free(values);
        }
    }
    if (st == MILENA_OK && active) {
        (void)milena_sql_rollback(c, NULL);
        err(e, MILENA_ERR_DATA, "Transacción SQL incompleta; cambios revertidos"); st = MILENA_ERR_DATA;
    }
    if (st == MILENA_OK && ferror(dest)) { err(e, MILENA_ERR_IO, "No se pudo escribir el resultado SQL"); st = MILENA_ERR_IO; }
    milena_sql_close(c);
    if (previous_sigint != SIG_ERR) (void)signal(SIGINT, previous_sigint);
    return st;
}
MilenaStatus milena_sql_run_program(const struct ASTNode *program, FILE *out, MilenaError *e) {
    MilenaSqlExecutionPlan plan = {0};
    MilenaStatus st = milena_sql_execution_plan_build(program, &plan, e);
    if (st == MILENA_OK) st = milena_sql_run_plan(&plan, out, e);
    milena_sql_execution_plan_destroy(&plan);
    return st;
}
