# Componente `common_types`

Provee los tipos base, utilidades de Tiempo Real (FreeRTOS/TWDT) y periféricos comunes de placa para todas las **ESP32-C6** y la **ESP32-S3**.

## Contenido
- [`CMakeLists.txt`](CMakeLists.txt): registra `status_led.c`, `str_metrics.c` y `boot_button.c` con sus dependencias (`esp_driver_gpio`, `esp_driver_rmt`, `esp_timer`, `freertos`, `nvs_flash`, `esp_system`).
- [`include/`](include/README.md): cabeceras públicas (`iot_common_types.h`, `status_led.h`, `str_metrics.h`, `boot_button.h`).
- [`src/`](src/README.md): implementaciones en C.
