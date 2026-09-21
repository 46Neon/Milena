# Arquitectura para procesamiento distribuido y rendimiento con SLO

## Corrección necesaria

Milena no puede garantizar una latencia fija para cualquier archivo y cualquier
infraestructura. El tiempo depende del volumen, almacenamiento, red, número de
workers, fallos, compactación y carga concurrente.

La garantía técnicamente válida debe ser un **SLO medible dentro de un perfil
de ejecución definido**:

- tamaño y formato de entrada;
- hardware y arquitectura;
- número de workers;
- memoria máxima por worker;
- ancho de banda mínimo de red y almacenamiento;
- paralelismo permitido;
- percentil objetivo, por ejemplo p95;
- comportamiento cuando el presupuesto no puede cumplirse.

La respuesta correcta no es prometer milisegundos fijos. Es rechazar o
cancelar una ejecución que no puede cumplir su contrato y devolver un reporte
verificable.

## Reestructuración propuesta

La ejecución distribuida debe continuar siendo una sola capacidad del
lenguaje:

```text
fuente Milena
  → lexer
  → parser
  → AST
  → semántica
  → plan lógico
  → plan físico
  → particiones
  → workers
  → shuffle/reducción determinista
  → reporte
```

No se debe añadir un segundo parser, un CLI paralelo ni una biblioteca externa
que interprete scripts fuera del runtime canónico.

### 1. Plan lógico

El AST validado se convierte en operaciones declarativas:

- lectura;
- proyección;
- filtro;
- transformación;
- agrupación;
- join;
- reducción;
- exportación.

El plan lógico no conoce máquinas, sockets ni procesos.

### 2. Plan físico

El runtime selecciona una estrategia ejecutable:

- una pasada local;
- particionado por rangos;
- particionado por hash;
- agregación parcial;
- shuffle por clave;
- reducción final determinista.

Cada operación debe declarar si es asociativa, conmutativa, orden-dependiente,
requiere materialización o necesita una segunda pasada.

### 3. Contrato de partición

Cada partición debe contener:

- identificador estable;
- rango o hash de origen;
- offset y longitud cuando el formato lo permita;
- checksum;
- esquema;
- versión del plan;
- límite de memoria;
- límite de tiempo;
- política de reintento.

CSV con registros entrecomillados no debe dividirse por offsets arbitrarios sin
un detector de límites de registro. Inicialmente, el particionado distribuido
debe priorizar formatos con fragmentación segura o crear índices de offsets.

### 4. Workers

Un worker solo recibe un plan físico tipado y una partición validada. Debe
producir:

- resultados parciales;
- filas aceptadas y rechazadas;
- bytes procesados;
- tiempo de CPU y pared;
- memoria máxima observada;
- checksum de salida;
- estado reproducible;
- razón de fallo o cancelación.

Los workers no deben ejecutar texto Milena ni cargar un parser propio.

### 5. Shuffle y reducción

Las agregaciones deben dividirse en:

```text
agregación parcial por worker
  → intercambio de particiones por clave
  → reducción final ordenada
```

Para resultados reproducibles, la reducción de floats debe utilizar un orden
determinista, acumuladores compensados o una representación decimal/exacta
cuando la semántica lo exija.

### 6. Coordinador y reanudación

El coordinador debe mantener un manifiesto de ejecución con:

- plan y versión;
- particiones pendientes, activas y terminadas;
- leases con expiración;
- intentos;
- checksums;
- checkpoints;
- presupuesto restante;
- estado final.

Un worker perdido debe poder reintentarse sin duplicar resultados. La salida
solo se publica después de validar todos los checksums y completar la
reducción final.

## Rendimiento con contrato verificable

El runtime debe aceptar un presupuesto de ejecución, no una promesa abstracta:

```text
memoria máxima por worker
workers requeridos
filas máximas
bytes máximos
presupuesto total de tiempo
percentil objetivo
política al exceder el presupuesto
```

El scheduler puede rechazar una ejecución si el SLO es imposible con los
recursos disponibles. Si se supera durante la ejecución, debe cancelar de
forma controlada y conservar un reporte parcial marcado como no exitoso.

La validación debe usar fixtures deterministas y medir:

- throughput de lectura;
- throughput total;
- p50, p95 y p99;
- memoria residente máxima;
- bytes de shuffle;
- skew de particiones;
- reintentos;
- tiempo de coordinación;
- consistencia frente a la ejecución local.

## Primer hito implementado en PR #27

El planner físico local ya está implementado en `partition_plan.c`. Recibe el tamaño del origen, un tamaño objetivo y un máximo de particiones; produce particiones contiguas con identificadores estables. La validación comprueba cobertura exacta, orden, ausencia de solapamientos, longitudes y límites.

Este hito permite declarar implementada la primera etapa de procesamiento distribuido: **planificación y validación local de particiones**. Todavía no ejecuta workers remotos ni introduce red.

## Orden de implementación

1. ~~Definir AST y plan lógico para operaciones distribuibles.~~ Preparar el contrato y la frontera canónica.
2. Añadir capacidades y propiedades de ejecución al runtime.
3. Crear planner físico local que produzca particiones deterministas.
4. Implementar ejecución de varias particiones dentro de un proceso para
   validar equivalencia sin introducir todavía red.
5. Añadir workers aislados y protocolo de resultados firmado por checksum.
6. Añadir shuffle y reducción determinista.
7. Añadir coordinador, leases y reanudación.
8. Ejecutar benchmarks reproducibles en hardware documentado.
9. Publicar SLO únicamente para perfiles medidos.

## Segundo hito implementado en PR #27

Sobre el planner se añadió `partition_executor.c`, un ejecutor local de
particiones en orden estable. Valida el plan, invoca un worker tipado por cada
partición y devuelve un reporte de completadas, fallidas, workers utilizados y
orden determinista. El contrato limita explícitamente esta fase a un worker
local: no finge paralelismo ni introduce sockets antes de validar la
semántica.

Esto permite probar equivalencia y manejo de errores en un proceso único antes
de añadir concurrencia o red.

## Tercer hito implementado en PR #27

Se añadió una prueba de equivalencia particionada que procesa un fixture
determinista por rangos y compara la reducción particionada con la reducción
monolítica. La prueba verifica que la cobertura del planner no pierde ni
duplica posiciones antes de añadir concurrencia o transporte de red.

## Estado honesto del proyecto

PR #27 implementa el primer componente distribuible: el planner físico local y
la validación de particiones. Esto permite declarar implementada la fase de
planificación local, no un clúster completo. El flujo de datos sigue siendo
local, de una pasada y de memoria acotada; aún faltan workers, red, shuffle,
coordinación, tolerancia a fallos y SLO medidos. La documentación distingue
explícitamente esas fases para no presentar una capacidad parcial como un motor
distribuido completo.
