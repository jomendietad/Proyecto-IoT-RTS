# Firmware Nodo 2 — Seguridad e Intrusión (`nodo2_seguridad_c6`)

Proyecto ESP-IDF (`target esp32c6`) para la placa **ESP32-C6-DevKitC-1**.

## Qué implementa
- Detección de movimiento por **interrupción GPIO** (`GPIO 3`, flanco de subida) desde el sensor **PIR HC-SR501**.
- **FSM de alarma tipo carro** de 3 estados (`DESARMADA`, `ARMADA`, `DISPARADA`) sin tiempo de gracia, con patrones visuales por `xTimer` en `GPIO 20`.
- Disparo instantáneo por PIR o tras **3 intentos consecutivos de RFID no autorizado**, LED RGB (`GPIO 8`) y `BOOT` (`GPIO 9`).
- En Fase 3 incorporará el servidor BLE GATT (`IOT-N2-SEGURIDAD`) para recibir armado/desarme desde Telegram o Nodo 1 (vía S3) y notificar disparos.

## Archivos y subcarpetas
- [`CMakeLists.txt`](CMakeLists.txt): declara el proyecto y enlaza `../../components`.
- [`sdkconfig.defaults`](sdkconfig.defaults): configura `esp32c6`, tick `1000 Hz`, estadísticas FreeRTOS y `TWDT` (`10 s`).
- [`main/`](main/README.md): código fuente C de la ISR del PIR, máquina de estados y consola de pruebas.
