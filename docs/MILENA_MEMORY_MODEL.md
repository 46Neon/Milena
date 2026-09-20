# Modelo de memoria de Milena

Este documento establece las reglas de ownership antes de introducir cualquier recolector limitado.

## Reglas base

- Quien crea un `MilenaArray` es responsable de liberar su referencia.
- `milena_array_retain` incrementa la vida útil de un arreglo compartido.
- `milena_array_release` libera la referencia y deja el descriptor en estado vacío cuando corresponde.
- Una vista comparte el almacenamiento, pero tiene su propio descriptor y debe liberar su propia referencia.
- Una operación que escribe en `out` debe recibir un descriptor válido y entregar su resultado con ownership para quien llamó.
- Los resultados temporales se liberan en el mismo ámbito que los creó.
- Un error después de una reserva debe liberar todas las reservas ya realizadas antes de regresar.

## Arreglos y vistas

```text
descriptor de arreglo
        ↓ retain/release
almacenamiento compartido
        ↓
memoria de datos
```

Una vista no debe liberar directamente la memoria de datos. Solo libera su referencia al almacenamiento compartido.

## Árboles y AST

Los nodos del AST y los nodos de árboles de decisión tienen ownership exclusivo durante su construcción. El contenedor que crea un nodo es responsable de destruirlo, salvo que lo transfiera explícitamente a otro contenedor.

## Temporales

Los temporales numéricos deben seguir este patrón:

```text
crear temporal
calcular
transferir resultado o liberar
```

No se debe conservar un puntero a datos de un temporal después de liberar su descriptor.

## Futuro recolector

Un recolector limitado podrá administrar objetos creados por scripts, como variables, listas, funciones y nodos semánticos. No reemplazará automáticamente el control explícito de los buffers numéricos grandes ni de las vistas.

Antes de incorporarlo deben existir pruebas de ownership, referencias compartidas, errores durante reservas y destrucción repetida.
