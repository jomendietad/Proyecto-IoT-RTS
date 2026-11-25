# Firmware Nodo 1 — Control de Acceso (`nodo1_acceso_c6`)

Proyecto ESP-IDF (`target esp32c6`) para la placa **ESP32-C6-DevKitC-1**.

## Qué implementa
- Lectura de tarjetas MIFARE con **RFID-RC522** sobre `SPI2` (`3.3V`, `GPIO 2/6/7/18/19`).
- Almacenamiento persistente de hasta 8 UIDs en **NVS** (UID por defecto: `B6:7B:0C:03`).
- Control enclavado (*toggle*) del LED de cerradura (`GPIO 20`), LED RGB (`GPIO 8`) y *factory reset* con `BOOT` (`GPIO 9`, 5 s).
- En Fase 3 incorporará el servidor BLE GATT (`IOT-N1-ACCESO`) para reportar eventos y recibir comandos del Maestro S3.

## Archivos y subcarpetas
- [`CMakeLists.txt`](CMakeLists.txt): declara el proyecto y enlaza `../../components`.
- [`sdkconfig.defaults`](sdkconfig.defaults): configura `esp32c6`, tick `1000 Hz`, estadísticas FreeRTOS y `TWDT` (`10 s`).
- [`main/`](main/README.md): código fuente C de tareas, drivers y consola de pruebas.
