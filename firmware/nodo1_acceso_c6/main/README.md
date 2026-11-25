# Código Fuente Nodo 1 (`nodo1_acceso_c6/main/`)

## Archivos
- [`CMakeLists.txt`](CMakeLists.txt): registra las fuentes y dependencias (`common_types`, `ble_protocol`, `esp_driver_spi`, `nvs_flash`, etc.).
- [`main.c`](main.c): crea `task_rfid_poll` (prio `12`, `T = 100 ms`, `D = 50 ms`, *cooldown* `1.2 s`), `task_hb_n1` (prio `3`, `15 s`) y `task_console_n1` (prio `2`, comandos `open`, `close`, `rfid list|add|del`, `sim_ok`, `sim_fail`, `status`, `stats`).
- [`rc522_spi.h`](rc522_spi.h) / [`rc522_spi.c`](rc522_spi.c): driver MFRC522 por `SPI2` (4 MHz); si no detecta hardware, opera en modo degradado permitiendo simulación por consola.
- [`rfid_store_nvs.h`](rfid_store_nvs.h) / [`rfid_store_nvs.c`](rfid_store_nvs.c): gestión *thread-safe* (`Mutex`) de UIDs en NVS (`rfid_store`), sembrando `B6:7B:0C:03` cuando la partición está vacía.
- [`lock_ctrl.h`](lock_ctrl.h) / [`lock_ctrl.c`](lock_ctrl.c): control protegido con `Mutex` del LED de cerradura en `GPIO 20`.
