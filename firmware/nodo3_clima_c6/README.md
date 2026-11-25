# Firmware Nodo 3 — Alertas Térmicas (`nodo3_clima_c6`)

Proyecto ESP-IDF (`target esp32c6`) para la placa **ESP32-C6 SuperMini** (compatible también con DevKitC-1).

## Qué implementa
- Lectura periódica no bloqueante del sensor **DHT11** (`GPIO 2`) cada `3 s` usando el periférico hardware **RMT RX** (sin *bit-banging* ni bloqueo de CPU).
- Detección de **alertas por sobresalto de temperatura**: cambio brusco `|ΔT| >= 2.0 °C` entre lecturas consecutivas y superación de umbral superior (`T_high = 30 °C` por defecto, histéresis `1.0 °C`, persistido en NVS).
- Señalización en LED RGB integrado (`GPIO 8`) y *factory reset* con `BOOT` (`GPIO 9`).
- En Fase 3 incorporará el servidor BLE GATT (`IOT-N3-CLIMA`) para notificar sobresaltos y recibir cambios de `/umbral` desde el Maestro S3.

## Archivos y subcarpetas
- [`CMakeLists.txt`](CMakeLists.txt): declara el proyecto y enlaza `../../components`.
- [`sdkconfig.defaults`](sdkconfig.defaults): configura `esp32c6`, tick `1000 Hz`, estadísticas FreeRTOS y `TWDT` (`10 s`).
- [`main/`](main/README.md): código fuente C del driver RMT, monitor térmico y consola de pruebas.
