# Cabeceras de `ble_protocol` (`include/`)

- [`ble_protocol.h`](ble_protocol.h):
  - **Identidad GAP/GATT:** nombres de Advertising (`IOT-N1-ACCESO`, `IOT-N2-SEGURIDAD`, `IOT-N3-CLIMA`), servicio `0xFF10` y características `0xFF11` (`Notify` C6→S3) y `0xFF12` (`Write with Response` S3→C6).
  - **Opcodes y Payloads `packed`:** tramas binarias en *little-endian* para alarma (`OP_ALARM_SET`, `OP_ALARM_EVENT`, `OP_RFID_AUTH_FAIL`), cerradura/RFID (`OP_LOCK_SET`, `OP_LOCK_STATUS`, `OP_RFID_MGMT`, `OP_RFID_MGMT_ACK`), clima (`OP_CLIMA_TELEMETRY`, `OP_THRESHOLD_SET`) y sistema (`OP_HEARTBEAT`, `OP_STATUS_REQ`, `OP_METRICS_REPORT`, `OP_PROTO_ACK`).
  - **Deduplicación:** estructura `ble_dedup_table_t` y firma `ble_dedup_check()`.
