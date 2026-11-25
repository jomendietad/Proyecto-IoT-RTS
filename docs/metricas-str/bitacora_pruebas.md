# Bitácora de Pruebas y Resultados Experimentales

Registro cronológico de validaciones en hardware real, mediciones de Sistemas en Tiempo Real (STR) y estado de avance por fase del proyecto.

---

## Resumen de Estado por Nodo y Fase

> **Fase 2 — Completada:** los 3 nodos `ESP32-C6` (`nodo1_acceso_c6`, `nodo2_seguridad_c6` y `nodo3_clima_c6`) quedaron validados en hardware real con métricas STR dentro de deadline (`misses=0`).

| Componente / Fase | Placa / Hardware | Estado | Última verificación |
| :--- | :--- | :--- | :--- |
| **Fase 2 — Nodo 2 (`nodo2_seguridad_c6`)** | `ESP32-C6-DevKitC-1` + PIR `HC-SR501` | **Validado en hardware** | Compilación + corrida continua `1022 s` |
| **Fase 2 — Nodo 1 (`nodo1_acceso_c6`)** | `ESP32-C6-DevKitC-1` + `RFID-RC522` | **Validado en hardware** | Compilación + pruebas SPI, NVS y `BOOT` |
| **Fase 2 — Nodo 3 (`nodo3_clima_c6`)** | `ESP32-C6 SuperMini` + `DHT11` | **Validado en hardware** | Compilación + corrida inicial `186 s` + corrida post-fix `75 s` sin errores |
| **Fase 3 — Red BLE GATT en Estrella** | `3× ESP32-C6` + `ESP32-S3 N16R8 UNO` | Pendiente | — |
| **Fase 4 — Coexistencia + Telegram** | `ESP32-S3 N16R8 UNO` | Pendiente | — |

---

## Fase 2 — Pruebas Individuales Standalone (FreeRTOS + Periféricos)

### 1. Nodo 2 — Seguridad e Intrusión (`nodo2_seguridad_c6`)

- **Hardware probado:** `ESP32-C6-DevKitC-1` (`rev v0.2`, `160 MHz`, `ESP-IDF v6.1.1`) + sensor PIR `HC-SR501` en `GPIO 3` + LED Alarma en `GPIO 20` + LED RGB `GPIO 8`.
- **Duración de corrida continua observada:** **`1022 s`** (~17 minutos sin reinicios ni disparos del TWDT).

#### Resultados funcionales verificados
- Arranque limpio con `TWDT` configurado a `10000 ms` (`panic=on`) y ventana de estabilización inicial del PIR (`5 s`).
- Movimiento PIR con alarma en **`DESARMADA`**: ignorado correctamente (`Movimiento PIR detectado (estado=DESARMADA, sin disparo)`).
- Transición a **`ARMADA`**: confirmación visual estilo carro y transición inmediata a **`DISPARADA`** ante flanco de subida del PIR (`reason=3`) o tras 3 fallos consecutivos de RFID simulados (`1/3`, `2/3`, `reason=4 fail=3`).
- Re-disparos del PIR estando ya en **`DISPARADA`**: filtrados sin corromper ni reiniciar el estado.
- Transición **`DISPARADA → DESARMADA`**: detiene el patrón de parpadeo y reinicia `fail_counter = 0`.

#### Métricas STR y estabilidad observadas
| Métrica | Valor observado | Límite / Referencia | Resultado |
| :--- | :--- | :--- | :--- |
| Latencia local `pir_to_led_us` | **`8 µs – 11 µs`** (`max_lat_us=11`) | `D = 10,000 µs` (10 ms) | **OK (`misses=0`)** |
| Heap libre (`free_heap` / `min_heap`) | `446,600 B` / `442,120 B` | Estable en `1022 s` | **Sin fugas** |
| Uso de CPU (`IDLE`) | **`98% – 99%`** | — | **Holgado** |
| Stack libre mínimo (`task_alarm_fsm`) | `3,404 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_hb_n2`) | `2,252 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_console_n2`) | `3,308 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |

---

### 2. Nodo 1 — Control de Acceso (`nodo1_acceso_c6`)

- **Hardware probado:** `ESP32-C6-DevKitC-1` (`rev v0.2`, `160 MHz`, `ESP-IDF v6.1.1`) + lector `RFID-RC522` por `SPI2` (`GPIO 2/6/7/18/19`, `3.3V`) + LED Cerradura en `GPIO 20` + LED RGB `GPIO 8` + botón `BOOT` `GPIO 9`.
- **Detección del lector SPI:** `RC522 inicializado (VersionReg=0xB2)`, comando `status` reportando `cerradura=CERRADA rc522=OK`.

#### Resultados funcionales verificados
- **Tarjeta autorizada por defecto (`B6:7B:0C:03`):** lecturas físicas por SPI alternaron limpiamente `CERRADA ↔ ABIERTA` en modo *toggle* enclavado, preparando `OP_ALARM_SET(DESARMADA)` al abrir y `OP_ALARM_SET(ARMADA)` al cerrar.
- **Rechazo de credenciales no autorizadas:**
  - Llavero RFID (`9A:A5:AE:02`): rechazado en múltiples intentos (`OP_RFID_AUTH_FAIL`) sin alterar el estado de la cerradura.
  - Teléfono móvil NFC (UID dinámico `08:90:FE:9C`, `08:6C:22:0D`, `08:CC:BA:02`, `08:9F:BA:5D`, `08:AD:48:7B`): rechazado correctamente.
- **Gestión dinámica NVS (`rfid list` / `rfid add` / `rfid del`):**
  - Alta de `9A:A5:AE:02` (`ESP_OK`) → pasó a abrir/cerrar la cerradura por SPI.
  - Baja de `9A:A5:AE:02` (`ESP_OK`) → volvió a ser rechazada de inmediato.
- **Comandos de consola (`open` / `close`):** control manual directo verificado sobre `GPIO 20`.
- **Factory reset por botón `BOOT` (5 s):** tras mantener presionado `BOOT` durante 5 s, se borró la partición NVS, reinició la placa y restauró únicamente el UID por defecto **`B6:7B:0C:03`** (rechazando nuevamente `9A:A5:AE:02`).

#### Métricas STR y estabilidad observadas
| Métrica | Valor observado | Límite / Referencia | Resultado |
| :--- | :--- | :--- | :--- |
| Latencia `rfid_to_lock_us` (SPI real) | **`37 µs – 105 µs`** (`max_lat_us=105`) | `D = 50,000 µs` (50 ms) | **OK (`misses=0`)** |
| Heap libre (`free_heap` / `min_heap`) | `439,608 B` / `435,324 B` | Estable tras múltiples ciclos y resets | **Sin fugas** |
| Uso de CPU (`IDLE`) | **`97% – 98%`** | — | **Holgado** |
| Stack libre mínimo (`task_rfid_poll`) | `3,284 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_hb_n1`) | `2,244 – 2,368 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_console_n1`) | `2,636 – 3,392 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |

---

### 3. Nodo 3 — Climatización / Alertas Térmicas (`nodo3_clima_c6`)

- **Hardware probado:** `ESP32-C6 SuperMini` (`ESP32-C6FH4`, `rev v0.2`, `160 MHz`, `ESP-IDF v6.1.1`, consola `USB Serial/JTAG`) + sensor `DHT11` en `GPIO 2` (`RMT RX` a `1 MHz`) + botón `BOOT` `GPIO 9`.

#### Corrida 1 (`186 s`) — Validación inicial e incidencia en `dht11_rmt.c`
- **Duración de corrida continua observada:** **`186 s`** (métricas periódicas consolidadas hasta `uptime_s=180`, sin reinicios ni disparos del TWDT).
- **Incidencia detectada y corregida en driver `dht11_rmt.c`:** durante la conexión inicial ocurrieron lecturas `ESP_ERR_INVALID_RESPONSE` antes de estabilizarse tras reinicio; se corrigió eliminando el glitch inicial en `GPIO 2`, ampliando el pulso de inicio a `22 ms` y `signal_range_max_ns` a `300 µs`, moviendo el buffer RMT a memoria estática y añadiendo recuperación automática (`rmt_disable`/`rmt_enable`) ante timeout o `ESP_ERR_INVALID_STATE`.
- **Resultados funcionales verificados:**
  - **Lectura física continua del `DHT11` vía `RMT RX` (`GPIO 2`):** lecturas periódicas cada `3,000 ms` reportando `sensor=OK` (`T = 29.0 °C – 34.6 °C`, `H = 89% – 95%`).
  - **Detección simultánea de umbral superior y sobresalto térmico:** ante calentamiento rápido del sensor se activó `ALERTA TERMICA [UMBRAL_Y_SOBRESALTO]` en `T=30.3 °C (dT=+2.8 °C) H=95%` (`seq=1`) y `T=33.0 °C (dT=+2.7 °C) H=89%` (`seq=2`), preparando notificaciones `OP_CLIMA_TELEMETRY`.
  - **Comando `status`:** verificado reportando estado real del sensor (`T=34.6C (dT=-0.1C) H=95% sensor=OK alerta=UMBRAL_ALTO T_high=30C`).
  - **Gestión dinámica del umbral con persistencia en NVS:** comando `umbral 50` actualizado en NVS (`ALERTA TERMICA [NORMAL]` en `T=29.0 °C`, `reason=3`, `seq=3`) y restauración posterior a `umbral 30`.

| Métrica (Corrida 1 — `186 s`) | Valor observado | Límite / Referencia | Resultado |
| :--- | :--- | :--- | :--- |
| Jitter periódico `dht11_jitter_us` (`T = 3,000 ms`) | **`102 µs – 970 µs`** (`avg_jit_us=969 µs` en corrida estable) | `D = 50,000 µs` (50 ms) | **OK (`misses=0`)** |
| Latencia evaluación alerta `temp_alert_eval_us` | **`13 µs – 18 µs`** (`max_lat_us=18 µs`) | `D = 50,000 µs` (50 ms) | **OK (`misses=0`)** |
| Heap libre (`free_heap` / `min_heap`) | `447,088 B` / `442,012 B` | Estable en `180 s` | **Sin fugas** |
| Uso de CPU (`IDLE`) | **`99%`** | — | **Holgado** |
| Stack libre mínimo (`task_temp_alert`) | `1,820 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_dht11_rmt`) | `2,308 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_hb_n3`) | `2,304 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_console_n3`) | `3,228 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |

#### Corrida 2 (`75 s`, post-fix) — Validación sin errores tras corrección en `dht11_rmt.c`
- **Duración de corrida continua observada:** **`75 s`** desde el primer arranque tras flashear el driver corregido, sin reinicios, sin errores de lectura y sin disparos del TWDT.
- **Resultados funcionales verificados:**
  - **Enganche inmediato del `DHT11` desde el arranque:** primera lectura física exitosa en `t = 3.256 s` (`T=25.8C (dT=+0.0C) H=72% sensor=OK alerta=NORMAL T_high=30C`), manteniendo lecturas cada `3,000 ms` sin ningún `ESP_ERR_INVALID_RESPONSE`, timeout ni error de CRC.
  - **Detección de sobresalto térmico (`[SOBRESALTO]`):** disparo en `T=27.9 °C (dT=+2.1 °C) H=95% T_high=30C reason=2` (`seq=1`), antes de alcanzar el umbral de `30 °C`.
  - **Detección combinada (`[UMBRAL_Y_SOBRESALTO]`):** disparo en `T=31.6 °C (dT=+2.2 °C) H=95% T_high=30C reason=2` (`seq=2`).
  - **Retorno automático por histéresis (`1.0 °C`):** transición a `ALERTA TERMICA [NORMAL]` en `T=28.9 °C (dT=-0.2 °C) H=95% T_high=30C reason=3` (`seq=3`).

| Métrica (Corrida 2 post-fix — `75 s`) | Valor observado | Límite / Referencia | Resultado |
| :--- | :--- | :--- | :--- |
| Jitter periódico `dht11_jitter_us` (`T = 3,000 ms`) | **`196 µs`** constante (`avg_jit_us=196 µs`) | `D = 50,000 µs` (50 ms) | **OK (`misses=0`)** |
| Latencia evaluación alerta `temp_alert_eval_us` | **`14 µs`** (`max_lat_us=14 µs`) | `D = 50,000 µs` (50 ms) | **OK (`misses=0`)** |
| Heap libre (`free_heap` / `min_heap`) | `446,840 B` / `441,904 B` | Estable en `75 s` | **Sin fugas** |
| Uso de CPU (`IDLE`) | **`99%`** | — | **Holgado** |
| Stack libre mínimo (`task_temp_alert`) | `2,296 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_dht11_rmt`) | `2,344 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_hb_n3`) | `2,336 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_console_n3`) | `3,312 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |

---

## Próximas Entradas (Fases 3 a 6)

- **Fase 3:** registro de conexiones BLE GATT en estrella (`IOT-S3-MAESTRO` ↔ `IOT-N1-ACCESO`, `IOT-N2-SEGURIDAD`, `IOT-N3-CLIMA`), reconexión automática y enrutamiento local `N1 → S3 → N2`.
- **Fase 4–5:** métricas de coexistencia Wi-Fi + BLE GATT en ESP32-S3, tiempos de Telegram y prueba *offline*.
- **Fase 6:** resultados consolidados de la matriz experimental `E1–E6`.
