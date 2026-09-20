# Modelo de memoria de `MilenaArray`

Este documento define el contrato de almacenamiento, vistas, mutabilidad y vida útil de la API pública declarada en `include/array.h`.

## 1. Cuatro niveles distintos

### Descriptor

`MilenaArray` es un descriptor por valor. Contiene dtype, forma, strides, tamaño lógico, offset, flags y un puntero a `MilenaArrayStorage`. El descriptor posee sus arreglos `shape` y `strides`: dos descriptores nunca deben liberar la misma metadata.

Un descriptor debe comenzar en **estado cero**, mediante `MilenaArray a = {0};` o `milena_array_init(&a)`. Después de `milena_array_release`, vuelve al estado cero. El estado cero es liberable e intercambiable, pero no representa un array y `milena_array_validate` lo rechaza.

No se debe copiar un `MilenaArray` con asignación o `memcpy`. Para obtener otro descriptor que apunte a los mismos elementos se usa `milena_array_share`.

### Storage

`MilenaArrayStorage` es privado. Contiene el inicio y extensión en bytes del buffer, un contador de referencias atómico, el deleter opcional y su contexto. La metadata del descriptor no vive en el storage.

El storage se retiene internamente antes de publicar una vista o un share. Se libera cuando desaparece la última referencia. Por ello, toda vista sobrevive a la liberación de su descriptor padre.

### Vista

Una vista tiene metadata propia y comparte storage. `byte_offset` identifica el elemento lógico de coordenadas cero; por tanto puede apuntar dentro del buffer. Los strides son bytes firmados: una vista invertida puede tener strides negativos y una vista broadcast usa strides cero.

`MILENA_ARRAY_OWN_DATA` se elimina al crear vistas o shares. No significa que una vista sea débil: el storage sigue retenido.

### Buffer

El buffer es la región `[data, data + nbytes)`. Los elementos lógicos pueden no ser contiguos y no tienen que aparecer en orden creciente. El validador calcula el menor y mayor byte alcanzable desde shape, strides y `byte_offset`, sin aritmética firmada desbordante.

## 2. Escalares y arrays vacíos

- Un escalar tiene `ndim == 0`, `shape == NULL`, `strides == NULL` y `size == 1`.
- Un array vacío tiene al menos una dimensión cero y `size == 0`.
- Un vacío válido conserva dtype, itemsize, shape, strides y storage, aunque su buffer interno tenga cero bytes y `data == NULL`.
- `milena_array_const_data` y `milena_array_data` pueden devolver `NULL` para un vacío de cero bytes. Esto no es error ni autoriza desreferenciarlo.
- La contigüidad de escalares y vacíos válidos es verdadera. La construcción evita strides cero accidentales en vacíos propios.

## 3. Invariantes comprobadas

`milena_array_validate` es el validador central y comprueba:

1. descriptor y storage presentes;
2. dtype válido e `itemsize == milena_dtype_size(dtype)`;
3. únicamente flags públicos conocidos;
4. metadata nula para escalares y completa para `ndim > 0`;
5. `size` igual al producto overflow-safe de `shape`;
6. alineación del primer elemento alcanzable y de los strides relevantes;
7. `byte_offset <= nbytes`;
8. buffer no nulo cuando existen bytes o elementos;
9. límites mínimo y máximo alcanzables para strides positivos, negativos y cero;
10. espacio completo para el último item;
11. regla global **stride cero implica `MILENA_ARRAY_READONLY`**.

Toda multiplicación o suma usada para tamaños, metadata, offsets y strides se comprueba. No se convierte un valor fuera de rango a `ptrdiff_t`.

La estructura de storage es opaca. Alterar manualmente campos públicos de un descriptor rompe el contrato; el validador intenta detectarlo antes de acceder al buffer.

## 4. Ownership y contador de referencias

El contador del storage es `atomic_size_t` C17. Los retains internos usan compare-and-exchange y rechazan la saturación en `SIZE_MAX`. El decremento final tiene semántica acquire/release y ejecuta el deleter exactamente una vez.

`milena_array_share(out, source, error)`:

- valida `source`;
- duplica `shape` y `strides`;
- retiene el storage;
- conserva readonly y offset;
- elimina `OWN_DATA` en el descriptor compartido;
- publica la salida solo al terminar correctamente.

`milena_array_retain` se conserva por ABI, pero está deprecado: solo retiene el storage y no crea metadata independiente. No convierte el mismo descriptor en dos objetos liberables. Código nuevo debe usar `milena_array_share`.

## 5. Buffers externos

`milena_array_from_buffer` adopta una descripción completa del buffer:

- `strides == NULL` solicita strides C-contiguos;
- un deleter no nulo transfiere ownership **solo si la llamada termina con éxito**;
- un deleter nulo crea storage prestado: el llamador mantiene vivo el buffer hasta liberar todas las vistas/shares;
- el deleter recibe exactamente `(data, deleter_context)` una vez al caer la última referencia;
- `MILENA_ARRAY_OWN_DATA` no se acepta como flag de entrada; es privado de las asignaciones internas;
- se aceptan escalares y vacíos;
- se validan alineación, offset, strides y límites antes del commit.

Si la construcción falla, el deleter no se llama y el llamador conserva el buffer.

## 6. Readonly y acceso a datos

Readonly es efectivo, no informativo:

- `milena_array_mut_data(array, &pointer, error)` deja `pointer == NULL` al entrar y rechaza descriptors inválidos o readonly;
- `milena_array_data` es el wrapper de compatibilidad y devuelve `NULL` para readonly o inválidos;
- `milena_array_const_data` permite lectura de arrays válidos;
- vistas normales heredan readonly;
- toda vista broadcast es readonly;
- todo descriptor con algún stride cero debe ser readonly.

La API no sincroniza escrituras al contenido del buffer. Readonly evita obtener un puntero mutable a través de esta API, pero no puede revocar un puntero externo que el llamador ya posea.

## 7. Slicing firmado

`MilenaSlice` representa `start`, `stop` opcionales y `step` firmado. `step` nunca puede ser cero. `milena_array_slice_view_ex` normaliza como Python/NumPy:

- índices negativos se miden desde el final;
- límites se recortan al dominio apropiado;
- con step positivo los defaults son `0:length`;
- con step negativo los defaults son `length-1:-1` (el stop `-1` default es sentinel, distinto de un `stop=-1` explícito);
- se calcula exactamente el número de seleccionados;
- el nuevo stride es `old_stride * step` con overflow comprobado;
- el offset se compone con el stride anterior, por lo que funciona sobre vistas ya invertidas.

`milena_array_slice_view` sigue disponible como wrapper legacy de límites `size_t`, step positivo y rango estricto.

## 8. Broadcasting zero-copy

`milena_array_broadcast_view` alinea dimensiones por la derecha. Cada dimensión origen debe coincidir con la destino o ser uno. Los ejes añadidos y expandidos reciben stride cero. La vista comparte storage, calcula el tamaño lógico con overflow y siempre es readonly.

Los kernels binarios existentes conservan su broadcasting propio y crean una salida materializada.

## 9. Aliasing y atomicidad frente a fallos

Todas las funciones públicas de `array.c` que producen arrays siguen el mismo patrón:

1. validan la salida previa y las entradas;
2. rechazan que `out` sea el mismo descriptor que una entrada;
3. rechazan que una salida previa comparta storage con una entrada;
4. construyen descriptor, metadata y/o buffer temporales;
5. ejecutan completamente la operación;
6. liberan la salida anterior y hacen commit mediante swap/asignación solo al final.

En cualquier error, la salida previa queda intacta. En éxito, una salida válida previa se reemplaza y libera. El contrato exige que `out` esté en estado cero o contenga un array válido creado por esta API.

Las entradas pueden compartir storage entre sí si la operación es puramente lectora; no se escribe sobre ellas.

## 10. Contigüidad

`milena_array_is_contiguous` primero valida el descriptor. Escalares y vacíos válidos son contiguos. En arrays no vacíos, exige orden C, stride positivo esperado en dimensiones de longitud mayor que uno y cálculos overflow-safe. Los strides de dimensiones singleton no afectan la contigüidad.

## 11. Thread-safety

- Retener/liberar **descriptores distintos** que comparten storage es seguro respecto al contador atómico y al deleter final.
- Un mismo descriptor, su metadata o su buffer no deben mutarse concurrentemente sin sincronización externa.
- Lecturas concurrentes son válidas si nadie modifica descriptor ni datos.
- El contador atómico no convierte los elementos del array en atómicos.
- El llamador debe garantizar que no se ejecutan simultáneamente dos releases sobre el mismo descriptor.

## 12. Compatibilidad

Las APIs existentes se conservan. El cambio intencional visible es que `milena_array_data` ya no entrega acceso mutable a readonly; código que necesite diagnosticar el motivo debe migrar a `milena_array_mut_data`. Los módulos existentes que escriben arrays propios continúan recibiendo un puntero mutable.
