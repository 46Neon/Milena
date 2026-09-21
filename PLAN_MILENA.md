# Plan de evolución de Milena

## Objetivo

Convertir Milena en un lenguaje y motor C17 de análisis de datos general, reproducible y eficiente, sin limitarlo al dominio SST. La visualización queda fuera de esta fase; Milena producirá datos tabulares, estadísticas, modelos y reportes que otras herramientas podrán visualizar.

## Fase 0 — Identidad y base estable

- Renombrar el proyecto, ejecutable, paquete, documentación y símbolos públicos a `Milena`.
- Adoptar `.milena` como única extensión oficial para programas y módulos.
- Crear una versión de transición con pruebas de regresión antes de añadir funciones nuevas.
- Actualizar la distribución y los instaladores después de validar el nuevo nombre.
- Mantener licencias, historial limpio, compilación reproducible y pruebas con GCC, Clang, ASan y UBSan.

## Fase 1 — Núcleo de datos general

- Tipos primitivos: entero, real, booleano, texto, fecha/hora y nulo.
- Tablas con nombres de columnas, tipos, índices y metadatos.
- Carga y exportación de CSV y JSON; después formatos columnares si la implementación lo justifica.
- Selección, filtrado, ordenamiento, unión, agrupación, agregación, pivote y transformación de columnas.
- Valores faltantes, duplicados, conversiones seguras y errores explicables.
- Límites de memoria, lectura por lotes y procesamiento incremental para archivos grandes.

## Fase 2 — Estadística y calidad

- Estadística descriptiva y cuantiles.
- Covarianza, correlación y tablas de contingencia.
- Pruebas inferenciales con supuestos y advertencias explícitas.
- Muestreo, intervalos de confianza y detección de valores atípicos.
- Pruebas de referencia contra resultados conocidos y comparación de precisión numérica.

## Fase 3 — Lógica financiera

- Decimal exacto o representación monetaria segura, separada de `double`.
- Monedas, redondeo, impuestos, descuentos y tasas con reglas explícitas.
- Fechas de pago, flujos de caja, amortización, valor presente y valor futuro.
- Validación de unidades, periodos y convenciones contables.
- Casos de prueba con valores de referencia y advertencias sobre límites legales o contables.

## Fase 4 — Rendimiento y arquitectura

- Separar el lenguaje, el motor de tablas, la estadística y los módulos de dominio.
- Evaluar almacenamiento columnar y ejecución vectorizada sin copiar datos innecesariamente.
- Paralelismo controlado cuando la semántica sea determinista.
- Benchmarks públicos frente a cargas pequeñas, medianas y grandes.
- API modular para agregar nuevos formatos y algoritmos.

## Fase 5 — Inteligencia artificial responsable

- Estadísticas de diagnóstico antes de usar modelos.
- Regresión, clasificación, agrupamiento y reducción de dimensión como módulos separados.
- Semillas reproducibles, particiones de datos y métricas de evaluación.
- Explicación de variables, advertencias de sesgo y detección de fuga de información.
- Integración opcional con modelos externos; el núcleo debe seguir funcionando sin conexión ni servicios propietarios.
- No presentar predicciones como causalidad ni como decisiones profesionales automáticas.

## Fase 6 — Sintaxis

La sintaxis se revisará después de estabilizar el modelo de datos y la lógica financiera. La extensión `.milena` seguirá siendo la base oficial del lenguaje.

## Fase 7 — Madurez para Termux

- Documentación clara de instalación y ejemplos generales.
- Releases regulares y changelog.
- Pruebas automáticas para aarch64, arm, i686 y x86_64 cuando estén disponibles.
- Issues reproducibles, colaboradores externos y revisiones públicas.
- Paquete reproducible sin root, sin binarios incluidos en el repositorio y con licencia MIT.
- Solicitar nuevamente la revisión oficial cuando Milena tenga actividad y adopción más allá de un proyecto personal.

## Principios

1. No prometer funciones que todavía no estén implementadas.
2. Preferir resultados reproducibles sobre automatismos opacos.
3. Separar el núcleo general de los módulos SST y financiero.
4. No incluir visualización en el núcleo durante esta etapa.
5. Mantener compatibilidad documentada durante las migraciones.

## Empaquetado Termux y APT (PR23)

- Mantener el nombre Milena en scripts, workflows, secretos, artefactos y documentación de empaquetado.
- Conservar una sola fuente ejecutable para publicación: `.github/workflows/publish-apt.yml`.
- Publicar por ahora únicamente un `.deb` Termux/aarch64 construido y probado en Termux; no declarar arquitecturas sin artefacto real.
- Rechazar explícitamente paquetes Debian/Ubuntu en el generador APT y verificar rutas bajo `$PREFIX`.
- Fijar timestamps mediante `SOURCE_DATE_EPOCH`, publicar SHA-256 y firmar `Release`, `InRelease` y `Release.gpg` sin exponer secretos.
- Tratar el repositorio APT y cualquier hosting como preparación: no anunciar instalación desde un gestor hasta probar una instalación real desde el repositorio remoto.
- Completar la checklist de construcción, instalación, actualización, eliminación, claves, hashes y revisión de Termux antes de enviar una nueva solicitud oficial.

## PR #26 — Binario canónico de Milena

Esta fase mejora las capacidades del binario canónico sin crear un runtime paralelo ni convertir Termux en una implementación separada. Toda capacidad nueva debe recorrer la arquitectura oficial:

```text
lexer → parser → AST → semántica → runtime → MilenaTable
```

Objetivos iniciales:

- mejorar las capacidades de análisis de datos del ejecutable `milena`;
- conservar la sintaxis española y hacerla progresivamente más humana;
- mantener resultados reproducibles y mensajes en español;
- medir compilación, arranque, memoria y ejecución antes de declarar mejoras de rendimiento;
- conservar la compatibilidad con Termux/TUR mediante releases versionadas;
- evitar rutas legacy, herramientas externas y módulos experimentales desconectados;
- añadir pruebas end-to-end para cada capacidad integrada.

El paquete Termux/TUR debe consumir únicamente releases estables producidas por este repositorio.
