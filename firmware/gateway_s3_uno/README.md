# Firmware Gateway / Maestro BLE GATT (`gateway_s3_uno`)

Proyecto ESP-IDF (`target esp32s3`) para la placa **ESP32-S3 N16R8 (formato UNO)**.

## Estructura implementada (Fase 3)
- `CMakeLists.txt` y `sdkconfig.defaults`: configuración `esp32s3` con NimBLE en modo **Central** fijado en **Core 0** (`MAX_CONNECTIONS=3`, `MTU=128`) y Flash `16 MB`.
- `main/Kconfig.projbuild`: configuración de pines `GW_RGB_GPIO` (defecto `48`) y `GW_BOOT_GPIO` (defecto `0`).
- `main/ble_central_star.c/.h` (**Core 0**): escaneo automático de `IOT-N1-ACCESO`, `IOT-N2-SEGURIDAD` e `IOT-N3-CLIMA`, descubrimiento del servicio `0xFF10`, suscripción automática CCCD a `0xFF11` y envío fiable *Write with Response* a `0xFF12`.
- `main/star_coordinator.c/.h` (**Core 1**): tabla global de estado, cola inter-núcleo, supervisión de *heartbeat* (`45 s`), LED RGB unificado y enrutamiento offline estilo carro `Nodo 1 → S3 → Nodo 2`.
- `main/main.c`: arranque FreeRTOS, Watchdog TWDT multi-core (`10 s`), métricas STR y consola serie interactiva (`status`, `net`, `arm`, `disarm`, `lock open|close`, `rfid list|add|del`, `umbral <C>`, `metrics`).

## Pendiente en Fase 4
- Activación de **Octal PSRAM (`N16R8`)**, coexistencia **Wi-Fi + BLE (`esp_coex`)**, failover dual Wi-Fi (`secrets.h`) y cliente HTTPS del **Bot de Telegram** (`/panel`).
