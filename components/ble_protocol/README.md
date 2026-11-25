# Componente `ble_protocol`

Define el contrato de comunicación binario compartido entre los **3 periféricos ESP32-C6** y el **Maestro ESP32-S3** sobre **BLE GATT en estrella (NimBLE)**.

## Contenido
- [`CMakeLists.txt`](CMakeLists.txt): compila `src/ble_protocol.c` enlazando `common_types`.
- [`include/`](include/README.md): constantes GATT (`0xFF10`, `0xFF11`, `0xFF12`), opcodes (`0xC0–0xCE`) y estructuras `packed` validadas con `_Static_assert`.
- [`src/`](src/README.md): lógica de deduplicación e idempotencia por `seq_num`.
