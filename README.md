<p align="center">
  <img src="milena-cover.jpg" alt="Milena — lenguaje para análisis de datos" width="100%">
</p>

# 🌿 Milena

## Análisis de datos intuitivo y nativo en español

Milena es un lenguaje y motor para análisis de datos, estadística y computación científica. Permite trabajar con arreglos, tablas y operaciones reproducibles mediante scripts con extensión `.milena`.

Milena se construye con una visión de largo plazo: convertirse en una herramienta clave para el análisis de datos, con una sintaxis clara para las personas, un motor controlable y una evolución orientada a la computación científica. Esa visión se desarrolla paso a paso, con capacidades verificadas antes de presentarlas como terminadas.

## 🚀 Empezar ahora

### Un primer script

```milena
arreglo valores = [1, 2, 3, 4];

forma(valores);
tamaño(valores);
media(valores);
mediana(valores);
percentil(valores, 90);
```

### Arreglos por eje

```milena
arreglo matriz = ceros(2, 3);

media(matriz, eje 0);
mediana(matriz, eje 1);
percentil(matriz, 90, eje 0);
```

Para conservar la dimensión reducida:

```milena
media(matriz, eje 0, conservar dimensiones);
mediana(matriz, eje 1, conservar dimensiones);
```

> [!TIP]
> Los ejemplos nuevos deben usar las palabras españolas. Durante la transición pueden existir nombres históricos compatibles, pero la sintaxis española es la dirección oficial del lenguaje.

## ¿Qué puede hacer Milena?

- Crear arreglos numéricos y arreglos de ceros.
- Consultar forma, dimensiones y tamaño.
- Ejecutar operaciones entre arreglos y escalares.
- Aplicar broadcasting en operaciones compatibles.
- Calcular suma, media, mínimo, máximo, varianza y desviación estándar.
- Calcular medianas y percentiles mediante interpolación lineal.
- Reducir operaciones por eje y conservar dimensiones.
- Trabajar con vistas, strides, reshape y transposición desde el motor de arreglos.
- Analizar archivos tabulares y generar reportes reproducibles.
- Ejecutar módulos de análisis estadístico y preventivo con advertencias explícitas.
- Iniciar el desarrollo de árboles de decisión y bosques clasificadores.

## 📦 Instalación y uso

Desde el repositorio:

```bash
git clone https://github.com/46Neon/Milena.git
cd Milena
./build.sh
```

Ejecutar un script:

```bash
./milena run examples/estadistica.milena
```

Analizar un archivo tabular:

```bash
./milena analizar datos.csv reporte.json
./milena perfil datos.csv perfil.json
```

La forma exacta de algunos comandos de archivos y reportes continúa evolucionando junto con el lenguaje. Los scripts deben conservar los datos de entrada, las reglas de limpieza y la versión del motor para facilitar la reproducción del análisis.

## ✍️ Sintaxis esencial

Las declaraciones y operaciones principales utilizan palabras españolas:

```milena
arreglo datos = [10, 20, 30, 40];

suma(datos);
media(datos);
minimo(datos);
maximo(datos);
varianza(datos);
desviacion_estandar(datos);
mediana(datos);
percentil(datos, 95);
```

Las reducciones por eje siguen una estructura explícita:

```milena
media(datos, eje 0);
media(datos, eje 0, conservar dimensiones);
media(datos, eje 0, sin conservar dimensiones);
```

La sintaxis busca ser humana en significado, pero conserva delimitadores claros para facilitar el análisis, los mensajes de error y la reproducibilidad. Las frases completamente libres todavía forman parte de una etapa futura.

## 🌲 Bosques en desarrollo

Milena ya cuenta con una primera base interna para clasificación mediante un conjunto de árboles simples y votación de clases. Esta capacidad todavía está en desarrollo y aún no se presenta como un sistema completo de aprendizaje automático.

El trabajo previsto incluye:

1. Separar árbol y bosque como componentes independientes.
2. Añadir profundidad configurable.
3. Incorporar más de dos clases.
4. Añadir selección reproducible de características.
5. Incorporar muestras de entrenamiento controladas.
6. Validar datos imperfectos y casos límite.
7. Exponer la capacidad mediante sintaxis española estable.

## 🧭 Roadmap

### Base disponible

- Arreglos numéricos.
- Formas, dimensiones y tamaño.
- Strides, vistas, reshape y transposición.
- Broadcasting.
- Estadística global y por eje.
- Medianas y percentiles.
- Reportes y análisis tabular.
- Validación automatizada en Linux y Windows.

### En desarrollo

- Bosques clasificadores.
- Pruebas multidimensionales más amplias.
- Sintaxis española semántica.
- Álgebra lineal ampliada.
- Gestión de memoria optimizada.

### Etapas futuras

- Integración numérica.
- Optimización científica.
- Diferenciación automática.
- Mejoras específicas para cada plataforma.
- Herramientas avanzadas de modelado.

## 🔍 Estado del proyecto

Milena está en desarrollo activo. Sus capacidades se incorporan por capas y se validan con pruebas automatizadas. Algunas operaciones, partes de la sintaxis y módulos avanzados todavía pueden cambiar.

> [!WARNING]
> Milena no sustituye una auditoría, una investigación profesional, una decisión médica, legal o financiera, ni una validación especializada. Los resultados deben interpretarse según los datos, el método utilizado y el contexto del análisis.

La visualización, los sistemas distribuidos y los modelos avanzados todavía forman parte de etapas posteriores.

## 🏗️ Organización del lenguaje

```text
script .milena
      ↓
lexer y parser
      ↓
representación semántica
      ↓
motor de arreglos y tablas
      ↓
operaciones estadísticas
      ↓
resultado o reporte
```

La forma en que una persona escribe una operación está separada de la implementación interna que la ejecuta. Esto permite mejorar la sintaxis sin reescribir los cálculos fundamentales.

## 🤝 Contribuir

Puedes probar Milena, revisar los ejemplos, reportar errores o proponer mejoras en el repositorio:

[Repositorio de Milena](https://github.com/46Neon/Milena)

Al reportar un problema, incluye cuando sea posible:

- sistema utilizado;
- script `.milena` mínimo que reproduce el problema;
- resultado esperado;
- resultado obtenido;
- versión o commit del proyecto.

Las contribuciones deben mantener la portabilidad, los errores explícitos, las pruebas automatizadas y la claridad de la sintaxis.

## 📄 Licencia

Milena se distribuye bajo la licencia MIT.
