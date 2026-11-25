# Firmware Gateway / Maestro BLE GATT (`gateway_s3_uno`)

Proyecto ESP-IDF (`target esp32s3`) destinado a la placa **ESP32-S3 N16R8 (formato UNO)** *(se implementará en las Fases 3 y 4)*.

## Cómo funcionará
- **Core 0 (Radio y Protocolos):** ejecutará **NimBLE en modo Central/Maestro** manteniendo 3 conexiones BLE GATT simultáneas (`IOT-N1-ACCESO`, `IOT-N2-SEGURIDAD`, `IOT-N3-CLIMA` sobre el servicio `0xFF10`), junto con el controlador **Wi-Fi**, LwIP y el árbitro de coexistencia `esp_coex`.
- **Core 1 (Aplicación y Red Superior):** alojará la tabla de estado global, el enrutamiento local estilo carro `Nodo 1 → S3 → Nodo 2` (operativo incluso sin Internet), el failover dual Wi-Fi (`secrets.h`), las colas priorizadas TX/RX y el cliente HTTPS (*Long Polling* + *Inline Keyboard* `/panel`) hacia la **API del Bot de Telegram**.
- **Memoria (`N16R8`):** habilitará los 8 MB de **Octal PSRAM** para buffers mbedTLS/HTTP/JSON, manteniendo stacks críticos en SRAM interna.
