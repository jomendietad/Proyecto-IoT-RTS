# Código Fuente Nodo 3 (`nodo3_clima_c6/main/`)

## Archivos
- [`CMakeLists.txt`](CMakeLists.txt): registra `main.c`, `dht11_rmt.c` y `temp_monitor.c` con dependencias (`common_types`, `ble_protocol`, `esp_driver_rmt`, `nvs_flash`, etc.).
- [`dht11_rmt.h`](dht11_rmt.h) / [`dht11_rmt.c`](dht11_rmt.c): canal `RMT RX` (`1 MHz`) sobre `GPIO 2` en drenador abierto + *pull-up*; decodifica los 40 bits del DHT11 por interrupción (`xTaskNotifyFromISR`) y valida checksum.
- [`temp_monitor.h`](temp_monitor.h) / [`temp_monitor.c`](temp_monitor.c): evalúa `|ΔT| >= 2.0 °C` y `T >= T_high` (con histéresis de `1.0 °C`), protegiendo el estado con `Mutex` y persistiendo `T_high` en NVS (`clima_cfg`).
- [`main.c`](main.c): crea `task_temp_alert` (prio `11`, `D = 50 ms`), `task_dht11_rmt` (prio `6`, `T = 3 s` con `vTaskDelayUntil` y medición de *jitter*), `task_hb_n3` (prio `3`) y `task_console_n3` (comandos `umbral <C>`, `sim_temp <C>`, `status`, `stats`).
