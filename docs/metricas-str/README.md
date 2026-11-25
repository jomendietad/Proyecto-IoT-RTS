# Diseño RTOS y Métricas de Tiempo Real (`docs/metricas-str/`)

Documenta el enfoque formal de **Sistemas en Tiempo Real (STR)** y registra los resultados medidos en hardware:

- [`diseno_rtos_str.md`](diseno_rtos_str.md): sustentación de **FreeRTOS (ESP-IDF v6.1.1) sobre Zephyr**, tablas de tareas/prioridades/deadlines/stacks de las 4 placas, *Core Pinning* en S3, TWDT e instrumentación 100 % por software (`[STR_METRIC]`, matriz `E1–E6`).
- [`bitacora_pruebas.md`](bitacora_pruebas.md): resultados experimentales reales de **Nodo 2** (`8–11 µs`, `1022 s`) y **Nodo 1** (`37–105 µs`, `VersionReg=0xB2`, NVS y `BOOT`), más estado de **Nodo 3**.
