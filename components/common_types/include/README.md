# Cabeceras Públicas de `common_types` (`include/`)

Contiene las interfaces C expuestas por el componente `common_types`:

- [`iot_common_types.h`](iot_common_types.h): enumeraciones de roles (`iot_node_role_t`), estados/causas de alarma (`iot_alarm_state_t`, `iot_alarm_reason_t`), cerradura (`iot_lock_state_t`), alertas térmicas (`iot_temp_alert_t`), límites (`T_high`, histéresis, TWDT) y helpers `*_str()`.
- [`status_led.h`](status_led.h): estados visuales del LED RGB WS2812 (`STATUS_LED_DISCONNECTED`, `STATUS_LED_CONNECTING`, `STATUS_LED_CONNECTED`, `STATUS_LED_ALARM_TRIGGERED`, etc.).
- [`str_metrics.h`](str_metrics.h): API de configuración TWDT (`str_twdt_init`), registro de métricas en µs (`str_metrics_record`) y reporte de salud/stack/CPU (`str_metrics_log_health`).
- [`boot_button.h`](boot_button.h): inicialización de la tarea de supervisión del botón `BOOT` para *factory reset* de NVS.
