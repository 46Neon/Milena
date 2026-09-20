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

Milena es un prototipo no registrado y tiene un responsable del proyecto que revisa los cambios del perfil y del repositorio. Las contribuciones externas no se incorporan automáticamente: deben ser evaluadas por el responsable antes de fusionarse. La revisión considera la necesidad del cambio, su seguridad, sus pruebas, su compatibilidad, su documentación y su coherencia con el alcance actual de Milena.

Enviar un Pull Request no concede autorización para publicar en `main`, modificar releases, usar la identidad visual como respaldo oficial ni representar una distribución como oficial. Solo los cambios aceptados mediante revisión forman parte del proyecto.

- No incluy datos reales de clientes, pacientes, trabajadores o empresas.
- Usa datos sintéticos para pruebas y ejemplos.
- No incluy claves, tokens, contraseñas ni archivos `.env`.
- No modifiques releases existentes.
- No subas binarios generados al repositorio fuente.
- Las decisiones SST deben conservar advertencias sobre causalidad, supuestos y revisión profesional.
- Los cambios en `main` requieren revisión del mantenedor.

## Calidad mínima

Los cambios de código deben compilar con C17 y conservar las advertencias estrictas del proyecto. Las nuevas funciones deben incluir pruebas y documentación cuando corresponda.

## Licencia

Al contribuir, aceptas que tu contribución se distribuya bajo la licencia MIT del proyecto.
