# Implementación de `ble_protocol` (`src/`)

- [`ble_protocol.c`](ble_protocol.c): implementa `ble_dedup_check()`, una tabla circular de 16 ranuras que registra el último `seq_num` procesado por pareja `(src_addr, opcode)` para descartar paquetes duplicados ante reconexiones BLE sin bloquear tareas.
