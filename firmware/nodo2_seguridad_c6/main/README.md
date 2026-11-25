# Código Fuente Nodo 2 (`nodo2_seguridad_c6/main/`)

## Archivos
- [`CMakeLists.txt`](CMakeLists.txt): registra las fuentes y dependencias (`common_types`, `ble_protocol`, `esp_driver_gpio`, `esp_timer`, etc.).
- [`pir_sensor.h`](pir_sensor.h) / [`pir_sensor.c`](pir_sensor.c): configura `GPIO 3` con *pull-down* e ISR `IRAM_ATTR` (`xTaskNotifyFromISR`), ventana de estabilización inicial (`5 s`) y antirrebote (`T_min = 500 ms`).
- [`alarm_fsm.h`](alarm_fsm.h) / [`alarm_fsm.c`](alarm_fsm.c): `task_alarm_fsm` (prio `16`, `D = 10 ms`) con `xTimer` para destello único al desarmar, doble destello al armar y parpadeo continuo indefinido al disparar; mide `pir_to_led_us`.
- [`main.c`](main.c): inicializa subsistemas, `task_hb_n2` (prio `3`, `15 s`) y `task_console_n2` (prio `2`, comandos `arm`, `disarm`, `sim_pir`, `sim_fail`, `status`, `stats`).
