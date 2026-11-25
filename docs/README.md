# Documentación Técnica del Proyecto (`docs/`)

Centraliza las especificaciones de arquitectura, hardware, protocolo BLE, entorno de desarrollo y análisis de Sistemas en Tiempo Real (STR).

## Subcarpetas y archivos
- [`entorno_desarrollo.md`](entorno_desarrollo.md): configuración de **ESP-IDF `v6.1.1`** en Linux (Fedora 45, Python 3.13 aislado), verificación agnóstica de permisos seriales/`udev` y flujo por terminal.
- [`hardware/`](hardware/README.md): mapeo GPIO de las 4 placas, restricciones eléctricas y código RGB.
- [`diagramas/`](diagramas/README.md): especificaciones del protocolo BLE GATT en estrella, FSMs y Bot de Telegram.
- [`metricas-str/`](metricas-str/README.md): diseño formal FreeRTOS/STR, experimentos `E1–E6` y bitácora de pruebas en hardware real.
