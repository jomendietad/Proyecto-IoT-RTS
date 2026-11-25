# Componentes Compartidos (`components/`)

Esta carpeta agrupa los componentes reutilizables de **ESP-IDF** compartidos por los cuatro proyectos de `firmware/` mediante `EXTRA_COMPONENT_DIRS "../../components"`.

## Subcarpetas
- [`common_types/`](common_types/README.md): enumeraciones del sistema, driver WS2812 RGB, instrumentación de métricas STR (`[STR_METRIC]`, TWDT) y botón `BOOT` de *factory reset*.
- [`ble_protocol/`](ble_protocol/README.md): definición del perfil BLE GATT en estrella (`0xFF10`), opcodes binarios (`0xC0–0xCE`), payloads `packed` y deduplicación por `seq_num`.

## Cómo funciona la integración
Al compilar cualquiera de los firmwares (`nodo1_acceso_c6`, `nodo2_seguridad_c6`, `nodo3_clima_c6` o el futuro `gateway_s3_uno`), CMake enlaza ambos componentes automáticamente para garantizar que emisor y receptor usen exactamente las mismas estructuras binarias y constantes temporales.
