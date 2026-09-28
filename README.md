<p align="center">
  <img src="assets/milena-logo-horizontal.jpg" alt="Milena — Lenguaje para análisis de datos" width="900">
</p>

<p align="center">
  <strong>Lenguaje de programación en español para convertir datos en análisis reproducibles.</strong>
</p>

<p align="center">
  <kbd>C17</kbd>
  <kbd>cross-platform</kbd>
  <kbd>data-analysis</kbd>
  <kbd>data-science</kbd>
  <kbd>milena</kbd>
  <kbd>programming-language</kbd>
  <kbd>scientific-computing</kbd>
  <kbd>statistics</kbd>
  <kbd>windows</kbd>
  <kbd>winget</kbd>
</p>

# Milena

## Una nueva experiencia de programar datos

Milena es un lenguaje de programación orientado al análisis de datos, la estadística y la computación científica, con una sintaxis española cada vez más humana.

Su propuesta no consiste solamente en traducir palabras clave del inglés. Milena busca cambiar la relación entre la persona y el análisis: que trabajar con datos se acerque a expresar una idea, no a descifrar una sucesión de comandos técnicos.

La dirección del proyecto es construir un lenguaje donde una persona pueda describir qué datos tiene, qué quiere limpiar, qué transformación necesita, cómo desea agruparlos, qué medida quiere calcular y qué resultado desea obtener, mediante una arquitectura de lenguaje real:

```text
script .milena
      ↓
lexer → parser → AST → semántica → runtime canónico
      ↓
arreglos, datasets, tablas, estadística y reportes
```

La sintaxis actual todavía utiliza paréntesis, llaves y símbolos porque permiten interpretar los programas con precisión. La evolución futura busca reducir progresivamente las concatenaciones y expresiones técnicas innecesarias, acercando el lenguaje a una forma más natural de describir análisis.

## La máquina del Excel, convertida en lenguaje

Excel demostró que una persona puede observar datos, transformarlos y obtener respuestas sin construir desde cero un sistema completo. Milena toma esa idea y la lleva hacia un lenguaje de programación.

Milena puede entenderse como una **máquina programable de análisis de datos**:

- las tablas representan la información;
- las transformaciones representan el razonamiento;
- las operaciones estadísticas representan las preguntas;
- el script representa el procedimiento completo;
- el reporte representa el resultado reproducible.

A diferencia de un flujo manual de celdas y clics, un programa Milena conserva qué archivo se utilizó, qué limpieza se aplicó, qué fórmulas se ejecutaron, cómo se agruparon los registros y qué reporte se generó.

La visión es combinar la accesibilidad conceptual de una hoja de cálculo con la reproducibilidad, automatización y control de un lenguaje de programación.

## Primer programa

```milena
.analisis resumen {
    arreglo valores = [1, 2, 3, 4];

    suma(valores);
    media(valores);
    mediana(valores);
    percentil(valores, 90);
}
```

Guárdalo como `estadistica.milena` y ejecútalo con:

```bash
./milena run estadistica.milena
```

## Capacidades actuales

Milena cuenta con una ruta canónica para:

- crear arreglos numéricos y arreglos de ceros;
- calcular suma, media, mínimo, máximo, varianza, desviación estándar, mediana y percentiles;
- ejecutar reducciones por eje y conservar dimensiones;
- cargar datasets CSV y declarar columnas numéricas, textuales, categóricas, binarias y de fecha;
- eliminar valores nulos y duplicados;
- crear columnas calculadas y extraer períodos desde fechas;
- filtrar, agrupar, resumir y unir datasets;
- seleccionar columnas y exportar reportes JSON;
- generar perfiles estadísticos e histogramas;
- ejecutar normalidad, tasas, Poisson, correlación, Wilcoxon y chi cuadrado;
- calcular riesgo relativo y odds ratio;
- generar modelos SST;
- ejecutar interés simple desde el lenguaje;
- procesar resúmenes numéricos CSV en modo flujo, sin materializar todas las filas;
- medir filas procesadas, filas válidas, errores y tiempo de la ejecución en flujo.

## Análisis de datasets

```milena
.analisis ventas {
    dataset cargar datos("datos/ventas.csv")

    variable precio numerica
    variable cantidad numerica
    variable fecha texto

    .limpiar dataset {
        #nulos("eliminar")
        #duplicados("eliminar")
    }

    .transformar dataset {
        #total("precio * cantidad")
        #periodo("mes de fecha")
    }

    .agrupar dataset {
        #por("periodo")
        #suma("total")
        #media("total")
        #conteo("total")
    }

    .exportar {
        ("reporte-ventas.json")
    }
}
```

Ejecútalo con:

```bash
./milena run ventas.milena
```

### Modo flujo para grandes CSV

Para resúmenes numéricos que no necesitan conservar toda la tabla, Milena ofrece
una ruta de memoria acotada:

```milena
.analisis ventas_masivas {
    datos desde "datos/ventas_masivas.csv"
        procesar por lotes de 4096 filas
        con registros de hasta 8 MiB

    resumir {
        suma de "importe";
        media de "importe";
        contar de "importe";
        varianza de "importe";
    }

    guardar resultado en "reporte_flujo.json"
}
```

El flujo lee el archivo secuencialmente, usa acumuladores de una pasada y no
crea un `Dataset` o `MilenaTable` con todas las filas. `con registros de hasta
8 MiB` es opcional y el runtime impone un tope duro de 64 MiB por registro.
El reporte expone límites, pico de búfer, filas y tiempo observado. El tiempo
real depende del tamaño del archivo y del almacenamiento: no es una garantía
de latencia fija.

La ruta admite `suma`, `media`, `minimo`, `maximo`, `conteo`, `varianza` y
`desviacion_estandar`, sin agrupaciones ilimitadas, joins, medianas, percentiles,
spill a disco ni procesamiento distribuido. La sintaxis legacy de PR24 sigue
siendo compatible. Consulta [la documentación del modo flujo](docs/STREAMING_EXECUTION.md).

## Instalación y uso

### Linux

Instala las herramientas de compilación:

```bash
sudo apt update
sudo apt install -y git build-essential
```

Descarga, construye y prueba Milena:

```bash
git clone https://github.com/46Neon/Milena.git
cd Milena
./build.sh
```

Uso directo:

```bash
./milena run examples/estadistica.milena
./milena vm programa_escalar.milena
./milena build programa_escalar.milena -o programa
./programa
./milena inspect datos.csv
./milena analizar datos.csv reporte.json
./milena perfil datos.csv perfil.json
```

`run` conserva la ruta existente del intérprete. `vm` compila a MLBC v1.2 desde la HIR escalar tipada canónica, verifica el bytecode y ejecuta esa entrada en la VM; muestra el resultado numérico en hexadecimal. `build <archivo.milena> -o <programa>` usa los mismos bytes verificados y emite un ejecutable nativo autónomo (no requiere después el fuente `.milena`) únicamente en Linux x86-64. En Windows, Termux/Android y otros destinos, `build` informa explícitamente que AOT no está soportado; la VM portable sigue disponible.

Estos dos comandos nuevos son una ruta experimental explícita, no compatibilidad completa con el lenguaje. El subconjunto exacto es una función `principal` sin parámetros, ayudantes escalares numéricos no recursivos, variables numéricas/booleanas locales, asignación, aritmética numérica, comparaciones admitidas, llamadas resueltas y control `si`/`sino` con condición booleana; todas las rutas deben retornar un número. Globales, HIR de datos/tablas, parámetros de `principal`, recursión y demás construcciones no representadas se rechazan sin redirigir al intérprete. Para esas fuentes, usa la ruta normal `run` cuando la gramática existente lo admita. Los detalles, compatibilidad v1.0/v1.1 y límites pendientes están en [BYTECODE v1](docs/BYTECODE_V1.md). La integración CLI no completa la fase 2 ni la fase 3 completa del plan.

También existe un constructor local de paquete Debian:

```bash
./packaging/debian/build-local-deb.sh
```

Este comando crea un paquete local para pruebas; no significa todavía que Milena esté publicada en un repositorio APT.

### Windows

Instala Git y LLVM/Clang:

```powershell
winget install LLVM.LLVM
```

Construye el ejecutable portable:

```powershell
git clone https://github.com/46Neon/Milena.git
cd Milena
.\packaging\windows\build.ps1
```

El ejecutable se genera en:

```text
dist\windows\milena.exe
```

Ejecuta un ejemplo:

```powershell
.\dist\windows\milena.exe run .\examples\estadistica.milena
```

En Windows también está disponible el modo experimental `vm` para el subconjunto
escalar documentado; `build` falla explícitamente porque el backend nativo está
limitado a Linux x86-64.

### Termux

Instala Git, Clang y Make:

```bash
pkg update
pkg install -y git clang make
```

Descarga y compila:

```bash
git clone https://github.com/46Neon/Milena.git
cd Milena
make
```

Ejecuta las pruebas y un ejemplo:

```bash
make test
./milena run examples/estadistica.milena
```

Termux utiliza las mismas fuentes C17 y el mismo pipeline canónico. La validación automatizada principal se ejecuta actualmente en Linux y Windows; cualquier diferencia específica de Android o Termux debe reportarse con el dispositivo, la versión de Termux y el commit utilizado.

## Instalación y empaquetado por plataformas

Los comandos objetivo, la arquitectura de cada artefacto, su disponibilidad y los requisitos pendientes se mantienen en la [matriz de instalación](docs/INSTALLATION_MATRIX.md). No se deben presentar como instalables desde un equipo limpio hasta que cada paquete esté indexado en una fuente consultable y haya pasado una prueba real de instalación.

| Plataforma | Comando objetivo | Estado |
|---|---|---|
| Windows x64 | `winget install --id 46Neon.Milena --exact` | Manifiestos adjuntos a una Release; publicación/indexación WinGet y prueba limpia pendientes. |
| Debian/Ubuntu Linux amd64 | `sudo apt install milena` | `.deb` local/de Release; repositorio APT Linux firmado y registrado pendiente. |
| Android/Termux AArch64 | `pkg install milena` | Receta candidata; aceptación/publicación y prueba en Android/Bionic pendientes. |

Los paquetes Linux y Termux son diferentes aunque ambos usen formato `.deb`: Linux usa su ABI y rutas `/usr`; Termux usa Bionic y `$PREFIX`. El workflow `.github/workflows/publish-apt.yml` corresponde al repositorio Termux/AArch64, no al canal APT de Debian/Ubuntu. Consulta la matriz para los gates y límites exactos por canal.

## Estado del proyecto

Milena ya es un runtime funcional y verificable, no solamente una idea conceptual ni una colección de ejemplos aislados. Cuenta con un pipeline lexer → parser → AST → semántica → runtime, ejecución de arreglos, datasets y tablas, análisis estadístico, operaciones SST, integración financiera inicial, reportes reproducibles y validación automatizada en Linux y Windows.

El proyecto se encuentra en una etapa de **consolidación avanzada del núcleo del lenguaje**. La arquitectura principal está implementada y validada, mientras que la sintaxis, las APIs y los módulos de alto nivel continúan evolucionando.

## Volumen de datos y alcance industrial

Milena trabaja principalmente con datasets y tablas cargados en memoria para transformaciones, joins y análisis completos. Además, PR24 incorpora una ruta de flujo para resúmenes numéricos CSV: esa ruta procesa el archivo secuencialmente y mantiene memoria acotada, sin materializar todas las filas.

El modo flujo no convierte automáticamente cualquier operación en streaming. Todavía no ofrece procesamiento distribuido, clústeres, joins externos ni garantías de latencia fija para volúmenes masivos. Leer todas las filas tiene un coste proporcional al archivo; los milisegundos se miden como observabilidad, no como una promesa universal.

Para convertirse en una plataforma preparada para grandes soluciones tecnológicas del mercado deberá ampliar el streaming a agrupaciones y joins externos, incorporar más formatos, medir benchmarks de alto volumen, mejorar la planificación, mantener políticas de memoria, añadir observabilidad y completar el empaquetado oficial.

La descripción más honesta es:

> Milena es un proyecto de ingeniería de lenguaje con un núcleo funcional avanzado, orientado a convertirse en una plataforma científica y de análisis de datos de alcance industrial.

No es solamente un ejercicio educativo, pero tampoco se presenta todavía como una plataforma industrial masiva ya consolidada. Es una base tecnológica real, verificable y en evolución hacia ese objetivo.

## Contribuir

Puedes contribuir mediante:

- ejemplos `.milena`;
- pruebas de integración;
- documentación;
- reportes de errores;
- mejoras del lexer, parser, AST y semántica;
- optimización del runtime;
- nuevos análisis estadísticos;
- mejoras de portabilidad;
- herramientas de empaquetado;
- propuestas para hacer la sintaxis más humana.

Al reportar un problema, incluye:

- sistema operativo;
- versión del compilador;
- comando utilizado;
- script mínimo que reproduce el problema;
- resultado esperado;
- resultado obtenido;
- commit utilizado.

Las contribuciones deben mantener la ruta canónica del lenguaje, la portabilidad C17, las pruebas automatizadas, los errores explícitos, la reproducibilidad y la claridad de la sintaxis.

## Licencia

Milena se distribuye bajo la licencia MIT.

[Repositorio oficial de Milena](https://github.com/46Neon/Milena)
