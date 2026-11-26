# Firmware Gateway / Maestro BLE GATT (`gateway_s3_uno`)

Proyecto ESP-IDF (`target esp32s3`) para la placa **ESP32-S3 N16R8 (formato UNO)**.

## Estructura implementada (Fases 3 y 4)
- `CMakeLists.txt` y `sdkconfig.defaults`: configuración `esp32s3` con Flash `16 MB` (`SINGLE_APP_LARGE`), **Octal PSRAM (`8 MB`)** para mbedTLS/HTTP/buffers de Telegram, NimBLE **Central** + Wi-Fi + LwIP fijados en **Core 0** y árbitro de coexistencia `esp_coex`.
- `main/Kconfig.projbuild`: configuración de pines `GW_RGB_GPIO` (defecto `48`) y `GW_BOOT_GPIO` (defecto `0`).
- `main/secrets.example.h`: plantilla versionada de credenciales Wi-Fi (primaria + respaldo) y Telegram (`TELEGRAM_BOT_TOKEN`, `TELEGRAM_AUTHORIZED_CHAT_ID`).
- `main/ble_central_star.c/.h` (**Core 0**): escaneo automático de `IOT-N1-ACCESO`, `IOT-N2-SEGURIDAD` e `IOT-N3-CLIMA`, descubrimiento del servicio `0xFF10`, suscripción automática CCCD a `0xFF11` y envío fiable *Write with Response* a `0xFF12`.
- `main/star_coordinator.c/.h` (**Core 1**): tabla global de estado, cola inter-núcleo, supervisión de *heartbeat* (`45 s`), LED RGB unificado, enrutamiento offline estilo carro `Nodo 1 → S3 → Nodo 2` y despacho de alertas hacia Telegram.
- `main/wifi_manager.c/.h` (**Core 1**): cliente Wi-Fi STA con `WIFI_PS_MIN_MODEM` y failover automático dual (red primaria ↔ respaldo tras 3 reintentos).
- `main/telegram_bot.c/.h` (**Core 1**): cliente HTTPS TLS persistente (`keep-alive` + `crt_bundle`), parser/constructor JSON en C puro sobre PSRAM, colas priorizadas (`PRIO_URGENT` / `PRIO_NORMAL`), validación estricta de `chat_id`, comandos de texto y `/panel` *Inline Keyboard*.
- `main/main.c`: arranque FreeRTOS, Watchdog TWDT multi-core (`10 s`), métricas STR y consola serie interactiva (`status`, `net`, `wifi`, `failover`, `panel`, `tg <msg>`, `arm`, `disarm`, `lock open|close`, `rfid list|add|del`, `umbral <C>`, `metrics`).

## Configuración de credenciales locales (`secrets.h`)
1. Copiar la plantilla sin salir del árbol ignorado por Git:
   ```bash
   cp firmware/gateway_s3_uno/main/secrets.example.h firmware/gateway_s3_uno/main/secrets.h
   ```
2. Editar `firmware/gateway_s3_uno/main/secrets.h` con tu SSID/clave Wi-Fi, `TELEGRAM_BOT_TOKEN` y `TELEGRAM_AUTHORIZED_CHAT_ID`.
3. Antes de recompilar tras el cambio de `sdkconfig.defaults`, regenerar `sdkconfig` con `rm -f firmware/gateway_s3_uno/sdkconfig && idf.py -C firmware/gateway_s3_uno reconfigure`.

## Estado de validación y pendientes
- **Fase 4 (Validada en placa):** compilación y flasheo en ESP32-S3 N16R8 UNO, coexistencia Wi-Fi + BLE (`3/3`), failover dual (`slot 0 ↔ slot 1` WPA2-PSK), `/panel` y comandos del Bot de Telegram verificados.
- **Fase 5–6 (Pendiente):** pruebas integrales de modo degradado/offline, gestión remota `/rfid` y matriz experimental `E1–E6`.
