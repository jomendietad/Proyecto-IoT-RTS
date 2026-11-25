# Herramientas Auxiliares y Análisis STR (`tools/`)

Esta carpeta alojará los scripts en **Python 3** para automatizar el análisis de las métricas de **Sistemas en Tiempo Real (STR)** durante la **Fase 6**.

## Cómo funcionará
- Leerá por puerto serie (o desde archivos de log capturados con `idf.py monitor`) las líneas estructuradas que imprimen los cuatro nodos:
  - `[STR_METRIC] node=... metric=... value_us=... deadline_us=... miss=...`
  - `[STR_HEALTH] ...`, `[STR_STACK] ...`, `[STR_CPU]`
- Calculará promedios, desviación estándar (*jitter*), percentiles (`p50`, `p95`, `p99`), peor caso observado (**WCRT**) y generará tablas comparativas/gráficas (ej. latencia con y sin coexistencia Wi-Fi) para la documentación final en `../docs/metricas-str/`.
