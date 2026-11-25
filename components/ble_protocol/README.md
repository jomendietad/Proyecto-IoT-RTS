# Componente `ble_protocol`

Define el contrato de comunicación binario y la capa de transporte compartida entre los **3 periféricos ESP32-C6** y el **Maestro ESP32-S3** sobre **BLE GATT en estrella (NimBLE)**.

## Contenido
- [`CMakeLists.txt`](CMakeLists.txt): compila `src/ble_protocol.c` y `src/ble_peripheral.c` enlazando `common_types` y `bt` (NimBLE).
- [`include/`](include/README.md):
  - `ble_protocol.h`: constantes GATT (`0xFF10`, `0xFF11`, `0xFF12`), opcodes (`0xC0–0xCE`), estructuras `packed` validadas con `_Static_assert` y tabla de deduplicación.
  - `ble_peripheral.h`: API reutilizable del GATT Server + Advertising para los nodos ESP32-C6.
- [`src/`](src/README.md): implementación de `ble_dedup_check()` y del servidor NimBLE Peripheral (`ble_peripheral.c`).
