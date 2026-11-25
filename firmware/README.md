# Proyectos de Firmware ESP-IDF (`firmware/`)

Contiene los cuatro proyectos independientes de **ESP-IDF `v6.1.1`**, uno por cada microcontrolador del sistema distribuido. Todos enlazan los componentes comunes de `../../components`.

## Subcarpetas
- [`nodo1_acceso_c6/`](nodo1_acceso_c6/README.md): **ESP32-C6-DevKitC-1** — Control de acceso (`RFID-RC522` + LED cerradura + NVS).
- [`nodo2_seguridad_c6/`](nodo2_seguridad_c6/README.md): **ESP32-C6-DevKitC-1** — Seguridad (`PIR HC-SR501` + FSM alarma tipo carro).
- [`nodo3_clima_c6/`](nodo3_clima_c6/README.md): **ESP32-C6 SuperMini** — Climatización (`DHT11` por RMT + alertas por sobresalto térmico).
- [`gateway_s3_uno/`](gateway_s3_uno/README.md): **ESP32-S3 N16R8 UNO** — Maestro BLE GATT + Border Router Wi-Fi + Bot Telegram *(Fases 3–4)*.

## Compilación estándar
Activa el entorno ESP-IDF y dentro de la carpeta del nodo ejecuta `idf.py build` (o `idf.py -p /dev/ttyACM0 flash monitor`).
