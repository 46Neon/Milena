# Contribuir a Milena

Gracias por tu interés en Milena. Las contribuciones son bienvenidas mediante Pull Requests.

## Flujo recomendado

1. Crea un fork del repositorio.
2. Crea una rama descriptiva, por ejemplo `fix/validacion-csv` o `feat/nueva-estadistica`.
3. Mantén cada cambio enfocado en un objetivo.
4. Ejecuta las pruebas antes de abrir el Pull Request:

```bash
make test
make clean
```

5. Describe qué cambió, cómo se probó y qué limitaciones quedan.
6. Abre un Pull Request contra `main`.

## Reglas del proyecto

- No incluy datos reales de clientes, pacientes, trabajadores o empresas.
- Usa datos sintéticos para pruebas y ejemplos.
- No incluy claves, tokens, contraseñas ni archivos `.env`.
- No modifiques releases existentes.
- No subas binarios generados al repositorio fuente.
- Las decisiones SST deben conservar advertencias sobre causalidad, supuestos y revisión profesional.
- Los cambios en `main` requieren revisión del mantenedor.

## Identidad visual

Antes de proponer o redistribuir recursos visuales, consulta [BRAND.md](BRAND.md). La procedencia y la licencia o el permiso aplicable deben quedar documentados; no asumas que la licencia MIT del código cubre estos archivos.

## Calidad mínima

Los cambios de código deben compilar con C17 y conservar las advertencias estrictas del proyecto. Las nuevas funciones deben incluir pruebas y documentación cuando corresponda.

## Licencia

Al contribuir, aceptas que tu contribución se distribuya bajo la licencia MIT del proyecto.
