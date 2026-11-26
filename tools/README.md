# Herramientas Auxiliares y Análisis STR (`tools/`)

Esta carpeta aloja el analizador y capturador de métricas de **Sistemas en Tiempo Real (STR)** para la **Fase 6**.

## Script disponible: `analyze_str_metrics.py`

Procesa líneas estructuradas de cualquiera de los 4 nodos (`[STR_METRIC]`, `[STR_HEALTH]`, `[STR_STACK]`), calculando `N`, mínimo, media, desviación estándar, percentiles (`p50`, `p95`, `p99`), peor caso observado (**WCRT**), *deadline misses* y porcentaje de cumplimiento.

> **Nota sobre `pyserial`:** para analizar archivos `.log` ya capturados basta `python3` del sistema. Para usar `--capture` por puerto serie, utiliza el Python del entorno de ESP-IDF (que ya incluye `pyserial`):
> ```bash
> IDF_PY="/home/johan/.espressif/tools/python/v6.1.1/venv/bin/python3"
> ```

### Ejemplos de uso (Experimentación `E1–E6`)

1. **Analizar logs ya capturados y exportar Markdown + CSV:**
   ```bash
   python3 tools/analyze_str_metrics.py exports/n2_e1.log exports/s3_e3.log \
     --csv exports/resumen_str.csv \
     --markdown exports/resumen_str.md
   ```

2. **Experimento E1 — 50 disparos automatizados en Nodo 2 (`pir_to_led_us`):**
   ```bash
   "$IDF_PY" tools/analyze_str_metrics.py --capture \
     --port /dev/ttyACM_N2 --duration 0 \
     --init-cmd "arm" \
     --repeat-cmd "sim_pir" --repeat-count 50 --repeat-interval 0.25 \
     --post-cmd "disarm" \
     --out exports/e1_nodo2_50x.log
   ```

3. **Experimento E5 — Captura continua del jitter DHT11 en Nodo 3 (10 min = 600 s):**
   ```bash
   "$IDF_PY" tools/analyze_str_metrics.py --capture \
     --port /dev/ttyACM_N3 --duration 600 \
     --out exports/e5_nodo3_10min.log
   ```

4. **Comparativa lado a lado (E3 sin Wi-Fi vs con Wi-Fi, o E4 red primaria vs hotspot):**
   ```bash
   python3 tools/analyze_str_metrics.py \
     --compare sin_wifi=exports/e3_sin_wifi.log con_wifi=exports/e3_con_wifi.log
   ```
