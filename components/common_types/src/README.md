# Implementación de `common_types` (`src/`)

- [`status_led.c`](status_led.c): driver WS2812 autocontenido sobre `driver/rmt_tx.h` (resolución `10 MHz`, codificador de bytes GRB, protegido con `Mutex` para uso seguro entre tareas).
- [`str_metrics.c`](str_metrics.c): inicializa/reconfigura el TWDT a `10 s`, mide tiempos y *deadline misses* con `esp_timer_get_time()` imprimiendo `[STR_METRIC]`, y reporta `[STR_HEALTH]`, `[STR_STACK]` (`uxTaskGetStackHighWaterMark`) y `[STR_CPU]` (`vTaskGetRunTimeStats`).
- [`boot_button.c`](boot_button.c): tarea periódica (`100 ms`, prio `4`, suscrita a TWDT) que detecta pulsación continua `>= 5 s` en `BOOT`, cambia el RGB a morado, borra NVS y reinicia el SoC.
