# Hoja de ruta técnica de Milena

Este documento convierte las limitaciones actuales en etapas implementables. Cada etapa debe conservar la arquitectura nativa C17 y mantener separados los datos homogéneos (`MilenaArray`), las tablas (`MilenaTable`) y los valores financieros exactos.

## Reglas de aceptación

- No representar dinero con `float64`.
- No copiar automáticamente arrays cuando una vista o un stride cero sea suficiente.
- Toda vista con broadcasting debe ser de solo lectura.
- Los valores nulos deben conservar una máscara de validez independiente.
- Cada operación debe comprobar dtype, shape, strides, overflow y ownership.
- Cada bloque debe incluir tests Linux y Windows antes de avanzar.
- Las funciones expuestas al lenguaje `.milena` deben reutilizar kernels del runtime; no duplicar lógica en el parser.
- Una capacidad se considera terminada solamente cuando existe API, ejecución `.milena`, errores y pruebas.

## Fase 1 — Exponer el runtime al lenguaje

1. Literales de arrays 1-D y multidimensionales.
2. Literales de dtype y shape.
3. Llamadas a `zeros`, `reshape`, `transpose`, `where` y reducciones.
4. Referencias a vistas y reglas de ownership.
5. Mensajes de error de dtype, shape y broadcasting.
6. Tests de programas `.milena` que creen y operen arrays.

**Salida:** los arrays dejan de estar disponibles solamente mediante la API C.

## Fase 2 — Núcleo vectorizado

1. Despacho centralizado de kernels.
2. Broadcasting completo, incluyendo máscaras.
3. Kernels aritméticos para dtypes numéricos.
4. Reducciones `prod`, `min`, `max`, `mean`, `argmin` y `argmax`.
5. Slicing con strides negativos.
6. Indexación por entero, rangos, listas y máscaras.
7. Operaciones complejas completas con reglas de promoción.
8. Tests de contigüidad, vistas, stride cero y arrays de solo lectura.

**Salida:** cálculo numérico general sin implementaciones duplicadas por dtype.

## Fase 3 — Tipos y tablas

1. `MilenaString` y almacenamiento de texto.
2. `MilenaCategorical` con diccionario de categorías.
3. Columnas de texto y categóricas en `MilenaTable`.
4. `join` interno, izquierdo y externo.
5. `pivot` y `unpivot`.
6. `group_by` con múltiples claves.
7. Varias agregaciones en una sola operación.
8. `fill_null` para todos los dtypes.
9. Ordenamiento y selección con texto, categorías y nulos.

**Salida:** tablas heterogéneas aptas para análisis real sin convertirlas en arrays numéricos artificiales.

## Fase 4 — Finanzas de calendario real

1. Conversión efectiva a periódica mediante raíces decimales con precisión solicitada.
2. Política explícita de convergencia y redondeo.
3. Fracciones reales de días en NPV y descuento.
4. Series de flujos irregulares con exponentes fraccionarios.
5. `XNPV` y `XIRR`.
6. Detección de múltiples soluciones de IRR.
7. Calendarios de días hábiles y feriados.
8. Pagos extraordinarios en amortizaciones.
9. Tasas variables por tramo.
10. Impuestos, comisiones y seguros como componentes separados del flujo.

**Salida:** modelos financieros reproducibles con fechas reales y reglas explícitas.

## Fase 5 — Estadística y aprendizaje

1. Estadística descriptiva avanzada.
2. Covarianza, correlación y cuantiles.
3. Distribuciones y muestreo reproducible.
4. Regresión lineal y regularizada.
5. Clasificación básica.
6. Métricas y validación cruzada.
7. Detección de fuga de información.
8. Explicabilidad, trazabilidad y controles contra sesgo.
9. Modelos de IA solamente después de estabilizar datos, estadísticas y validación.

## Orden inmediato de implementación

1. Exponer creación y operaciones básicas de `MilenaArray` desde `.milena`.
2. Incorporar un dispatcher de kernels para broadcasting y dtype.
3. Completar slicing negativo e indexación avanzada.
4. Añadir strings/categorías y columnas de texto.
5. Añadir joins, pivot y agrupaciones múltiples.
6. Completar el bloque financiero de fechas reales y flujos irregulares.
7. Implementar estadística avanzada.
8. Añadir regresión, clasificación e IA bajo controles de validación.

Cada commit debe indicar qué fase toca, qué API pública añade y qué pruebas la validan.
