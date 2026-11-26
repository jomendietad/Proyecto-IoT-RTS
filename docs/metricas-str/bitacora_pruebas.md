# Bitácora de Pruebas y Resultados Experimentales

Registro cronológico de validaciones en hardware real, mediciones de Sistemas en Tiempo Real (STR) y estado de avance por fase del proyecto.

---

## Resumen de Estado por Nodo y Fase

> **Fases 1 a 6 — Completadas:** los 3 nodos `ESP32-C6`, la red **BLE GATT en estrella**, la coexistencia **Wi-Fi + BLE (`esp_coex`) + Bot de Telegram**, la validación integral y la matriz experimental **STR (`E1–E6`)** quedaron validadas en hardware real y procesadas con `tools/analyze_str_metrics.py`.

| Componente / Fase | Placa / Hardware | Estado | Última verificación |
| :--- | :--- | :--- | :--- |
| **Fase 2 — Nodo 2 (`nodo2_seguridad_c6`)** | `ESP32-C6-DevKitC-1` + PIR `HC-SR501` | **Validado en hardware** | Compilación + corrida continua `1022 s` |
| **Fase 2 — Nodo 1 (`nodo1_acceso_c6`)** | `ESP32-C6-DevKitC-1` + `RFID-RC522` | **Validado en hardware** | Compilación + pruebas SPI, NVS y `BOOT` |
| **Fase 2 — Nodo 3 (`nodo3_clima_c6`)** | `ESP32-C6 SuperMini` + `DHT11` | **Validado en hardware** | Compilación + corrida inicial `186 s` + corrida post-fix `75 s` sin errores |
| **Fase 3 — Red BLE GATT en Estrella** | `3× ESP32-C6` + `ESP32-S3 N16R8 UNO` | **Validado en hardware** | Corrida continua `1782 s` (~30 min), reconexión `3/3` y ruta `N1→S3→N2` |
| **Fase 4 — Coexistencia + Telegram** | `ESP32-S3 N16R8 UNO` + `3× ESP32-C6` | **Validado en hardware** | Octal PSRAM, `esp_coex`, failover dual Wi-Fi (`slot 0 ↔ slot 1`), `/panel` y comandos Telegram |
| **Fase 5 — Integral, `/rfid` y Offline** | Sistema completo (`3× C6 + S3 + Telegram`) | **Validado en hardware** | Flujo completo, 3 fallos RFID, `/rfid` NVS, `/umbral`, sobresalto térmico, failover y reconexión `3/3` |
| **Fase 6 — Mediciones STR (`E1–E6`)** | 4 placas + `tools/analyze_str_metrics.py` | **Validado en hardware** | Matriz `E1–E6`: `WCRT E1=40 µs`, `E3 ΔWCRT=+1.1%`, `E4 p50=665.6/822.4 ms`, `E5 10 min σ=1.9 µs` |

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

## Fase 3 — Red BLE GATT en Estrella (`gateway_s3_uno` ↔ `3× ESP32-C6`)

- **Hardware probado:** `ESP32-S3 N16R8 UNO` (`rev v0.2`, `240 MHz`, `8 MB PSRAM`, `ESP-IDF v6.1.1`) actuando como Maestro/Central en **Core 0** + Coordinador en **Core 1**, conectado simultáneamente por BLE GATT (`0xFF10`) a `IOT-N1-ACCESO`, `IOT-N2-SEGURIDAD` e `IOT-N3-CLIMA`.
- **Duración de corrida continua observada:** **`1,782 s`** (~30 minutos continuos sin reinicios, fugas de heap ni disparos del TWDT).

#### Resultados funcionales verificados
- **Conexión simultánea `3/3` y descubrimiento GATT automático:** enlace con `IOT-N3-CLIMA` (`RSSI=-43 dBm`), `IOT-N2-SEGURIDAD` (`RSSI=-60 dBm`) e `IOT-N1-ACCESO` (`RSSI=-62 dBm`), negociación `MTU=128` y suscripción automática CCCD a `0xFF11`.
- **Reconexión automática tras desconexión (`reason=0x208`):** al caer los enlaces (`3/3 → 2/3 → 1/3 → 0/3`), el Maestro S3 reanudó el escaneo y restableció automáticamente la estrella completa (`3/3`) sin intervención manual.
- **Enrutamiento offline estilo carro (`N1 → S3 → N2`):**
  - Tarjeta autorizada (`B6:7B:0C:03`): alternó `CERRADA ↔ ABIERTA` en N1 y enrutó `OP_ALARM_SET(ARMADA/DESARMADA)` hacia N2, incluyendo el silenciado inmediato desde `DISPARADA` (`reason=1`, `fail=0/3`).
  - Credenciales no autorizadas (llavero `9A:A5:AE:02` y NFC móvil `08:...`): enrutamiento de `OP_RFID_AUTH_FAIL` hacia N2 con conteo `fail=1/3`, `fail=2/3` y disparo automático en el 3.er intento (`DISPARADA reason=4 fail=3/3`).
- **Disparo PIR sobre red en estrella:** detecciones físicas en N2 notificadas al instante a la S3 (`N2 ALARMA=DISPARADA reason=3 lat_local=16–21 us`).
- **Telemetría térmica de N3 por BLE:** recepción de transiciones `[SOBRESALTO]` (`T=29.0 °C`, `dT=+2.9 °C`), `[UMBRAL_Y_SOBRESALTO]` (`T=31.7 °C`, `dT=+2.7 °C`), `[UMBRAL_ALTO]` (`T=32.8 °C`) y retorno por histéresis a `[NORMAL]` (`T=28.9 °C`).
- **Consola S3 (`status` / `ESTADO GLOBAL`):** consulta consolidada verificada (`Cerradura=ABIERTA | Alarma=DESARMADA | Clima=OK ... | BLE=3/3`).

#### Métricas STR y estabilidad observadas en Maestro S3 (`1,782 s`)
| Métrica | Valor observado | Límite / Referencia | Resultado |
| :--- | :--- | :--- | :--- |
| Latencia de enrutamiento `s3_route_n1_to_n2_us` (`OP_RFID_AUTH_FAIL`) | **`1,115 µs – 1,185 µs`** | `D = 100,000 µs` (100 ms) | **OK (`misses=0`)** |
| Latencia de enrutamiento `s3_route_n1_to_n2_us` (`OP_ALARM_SET`) | **`3,289 µs – 4,741 µs`** (`max_lat_us=4,741`) | `D = 100,000 µs` (100 ms) | **OK (`misses=0`)** |
| Latencia local PIR reportada por N2 (`lat_local`) | **`16 µs – 21 µs`** | `D = 10,000 µs` (10 ms) | **OK (`misses=0`)** |
| Heap libre en S3 (`free_heap` / `min_heap`) | `264,108 B` / `260,388 B` | Estable en `1,776 s` | **Sin fugas** |
| Uso de CPU Dual-Core (`IDLE0` / `IDLE1`) | **`99%` / `99%`** | — | **Holgado** |
| Stack libre mínimo (`task_state_fsm_s3`) | `2,240 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_hb_s3`) | `1,620 B` (de `3,072 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_console_s3`) | `2,108 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |

---

## Fase 4 — Coexistencia Wi-Fi + BLE GATT, Failover Dual y Bot de Telegram (`gateway_s3_uno`)

- **Hardware y memoria probados:** `ESP32-S3 N16R8 UNO` (`16 MB Flash`, `8 MB Octal PSRAM` a `80 MHz`, `ESP-IDF v6.1.1`, tabla `SINGLE_APP_LARGE`, binario `0x137fc0` B) en coexistencia simultánea `esp_coex` (**Core 0**: NimBLE Central + Wi-Fi + LwIP; **Core 1**: FSM + `wifi_mgr` + Telegram HTTPS).
- **Redes probadas:** primaria (`slot 0`, `Familia_Mendieta_Dilbert.`, IP `192.168.1.12`) + respaldo hotspot móvil (`slot 1`, `POCO X4 GT`, WPA2-PSK, IP `10.213.237.133`, `RSSI=-48 dBm`).

#### Incidencias detectadas y corregidas durante la validación
1. **Fallo de failover hacia AP WPA2 puro (`reason=201` `NO_AP_FOUND`):** `wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK` (enum `4`) descartaba redes `WIFI_AUTH_WPA2_PSK` (enum `3`). Se corrigió a `WIFI_AUTH_WPA_PSK` (enum `2`) con `WIFI_ALL_CHANNEL_SCAN` y supresión de evento `reason=8` durante conmutación forzada.
2. **Consumo de CPU en `task_wifi_sup` (`52 %`) y `wifi` (`30 %`):** `xEventGroupWaitBits` incluía `WIFI_EVT_CONNECTED_BIT` con `xClearOnExit=pdFALSE`, provocando *busy-wait* al estar conectado. Tras retirarlo, `task_wifi_sup` bajó a **`<1 %`** y `wifi` a **`2 %`**.
3. **Optimización de respuesta Telegram y métrica `tg_alert_e2e_us`:** se eliminaron mensajes intermedios redundantes en `/armar`, `/desarmar` y `/cerradura`, se ajustó `getUpdates` a `timeout=15` y se restringió el conteo de `tg_alert_e2e_us` a alertas encoladas con Wi-Fi activo (evitando falsos *deadline misses* offline).

#### Resultados funcionales verificados
- **Bot de Telegram completo:** ejecución verificada de todos los comandos de texto (`/panel`, `/estado`, `/armar`, `/desarmar`, `/cerradura`, `/umbral`, `/rfid`, `/red`, `/metricas`), *Inline Keyboard* y recepción de notificaciones desde los 3 nodos C6.
- **Failover Wi-Fi en vivo (`slot 0 → slot 1`):** conmutación exitosa al primer intento hacia `POCO X4 GT` (`WPA2-PSK`, canal `1`) manteniendo **`BLE=3/3`** sin desconexiones.

#### Métricas STR y estabilidad observadas tras correcciones
| Métrica | Valor observado | Referencia | Resultado |
| :--- | :--- | :--- | :--- |
| Uso de CPU Dual-Core (`IDLE0` / `IDLE1`) | **`93% – 94%` / `92% – 94%`** | — | **Holgado** |
| CPU `task_wifi_sup` / `wifi` / `task_tg_tx` / `task_tg_rx` | `<1%` / `2%` / `1%–2%` / `1%–2%` | — | **OK** |
| Heap total libre (`free_heap` / `min_heap` incl. PSRAM) | `8,347,631 B – 8,400,475 B` / `8,261,104 B` | Estable | **Sin fugas** |
| Deadline misses (`misses`) | **`0`** | `D_urgent = 3,000 ms` | **OK** |
| Stack libre mínimo (`task_state_fsm_s3`) | `2,064 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_tg_tx` / `task_tg_rx`) | `4,460 B` / `4,340 B` (de `8,192 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_wifi_sup`) | `2,684 B` (de `4,096 B`) | ≥ 25 % libre | **OK** |
| Stack libre mínimo (`task_hb_s3` / `task_console_s3`) | `1,828 B` / `2,248 B` | ≥ 25 % libre | **OK** |

---

## Fase 5 — Validación Integral, Gestión Remota `/rfid` y Resiliencia (`21:11` – `22:21`)

- **Sistema completo probado:** `ESP32-S3 N16R8 UNO` + `3× ESP32-C6` (`IOT-N1-ACCESO`, `IOT-N2-SEGURIDAD`, `IOT-N3-CLIMA`) + grupo autorizado de Telegram.

#### Resultados funcionales verificados en sesión continua
- **Acoplamiento estilo carro (`N1 → S3 → N2`) y control bidireccional:**
  - Tarjeta autorizada (`B6:7B:0C:03`) alternó `ABIERTA ↔ CERRADA` en N1 y `DESARMADA ↔ ARMADA` en N2, incluyendo silenciado directo desde `DISPARADA`.
  - Comandos de texto (`/panel`, `/estado`, `/red`, `/metricas`, `/armar`, `/desarmar`, `/cerradura abrir|cerrar`, `/umbral`, `/rfid`) y botones *Inline Keyboard* ejecutados con confirmación en Telegram.
- **Disparos de alarma verificados:**
  - **PIR (`N2`):** múltiples disparos reportados a Telegram con latencia local medida en **`19 µs – 24 µs`** (`D = 10,000 µs`).
  - **3 intentos RFID consecutivos no autorizados:** disparos verificados tanto con NFC móvil (`08:2A:5A:1A`, `08:F1:B2:B7`, `08:C0:99:63`) como tras revocar el llavero (`9A:A5:AE:02`), activando `ALARMA DISPARADA (3/3)`.
- **Gestión dinámica `/rfid` en NVS del Nodo 1 desde Telegram:**
  - `/rfid list` → `1/8 tarjetas (B6:7B:0C:03)`.
  - `/rfid add 9A:A5:AE:02` → `2/8 tarjetas`; el llavero pasó inmediatamente a abrir/cerrar cerradura y armar/desarmar alarma.
  - `/rfid del 9A:A5:AE:02` → `1/8 tarjetas`; el llavero volvió a ser rechazado de inmediato y disparó alarma al 3.er intento.
- **Alertas térmicas y umbral dinámico (`/umbral`, Nodo 3):**
  - `/umbral 28` → detección combinada `UMBRAL_Y_SOBRESALTO` (`T=29.3 °C`, `dT=+2.9 °C`, `H=95%`, `Umbral=28 °C`).
  - `/umbral 35` → retorno por histéresis a `Temperatura normalizada` (`T=29.5 °C`, `dT=-0.3 °C`, `H=95%`, `Umbral=35 °C`).
- **Resiliencia de red y reconexión:**
  - Failover Wi-Fi ida y vuelta (`slot 0 → slot 1 POCO X4 GT IP 10.213.237.133 → slot 0 IP 192.168.1.12`, `Failovers: 2`) manteniendo **`BLE=3/3`**.
  - Desconexión y reconexión individual en caliente de `N3`, `N1` y `N2` (`DESCONECTADO (2/3 BLE)` → `ONLINE (3/3 BLE)`) con notificación al grupo.

#### Instantánea de `/metricas` consolidada en vivo desde Telegram
| Nodo | `max_lat` | `avg_jit` | `misses` | `min_stack` | Memoria libre |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **Gateway S3** | `0 us` | `0 us` | **`0`** | `1,660 B` | SRAM `93 KB` (`min=38 KB`) / PSRAM `8,058 KB / 8,192 KB` |
| **Nodo 1 (Acceso)** | `0 us` | `0 us` | **`0`** | `2,188 B` | Heap `352–353 KB` |
| **Nodo 2 (Seguridad)** | `19–24 us` (PIR) | `0 us` | **`0`** | `2,208 B` | Heap `359 KB` |
| **Nodo 3 (Clima)** | — | `202 us` | **`0`** | `2,148 B` | Heap `364 KB` |

---

## Fase 6 — Resultados Experimentales de Tiempo Real (`E1–E6`)

Todos los experimentos fueron capturados directamente desde los puertos serie de las 4 placas (`/dev/ttyACM0` N1, `/dev/ttyACM1` N2, `/dev/ttyACM2` N3 y `/dev/ttyUSB0` S3) y procesados con [`tools/analyze_str_metrics.py`](../../tools/analyze_str_metrics.py).

### Matriz consolidada de resultados (`E1–E5`)

| Exp. | Nodo / Condición | Métrica | `N` | Min | Media | Desv.Est | `p50` | `p95` | `p99` | **WCRT (Max)** | Deadline | Misses | Cumplimiento |
| :---: | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **E1** | **N2** — Disparo alarma (`arm→sim_pir`) | `pir_to_led_us` | `50` | `29 µs` | `33.6 µs` | `2.2 µs` | `33.0 µs` | `37.0 µs` | `39.5 µs` | **`40 µs`** | `10,000 µs` | **`0`** | **100.00%** |
| **E2** | **N1** — Acceso local sin Wi-Fi | `rfid_to_lock_us` | `30` | `105 µs` | `123.8 µs` | `7.8 µs` | `125.0 µs` | `134.2 µs` | `137.4 µs` | **`138 µs`** | `50,000 µs` | **`0`** | **100.00%** |
| **E2 / E3-A** | **S3** — Ruta `N1→S3→N2` **sin Wi-Fi** (`BLE=3/3`) | `s3_route_n1_to_n2_us` | `30` | `1,043 µs` | `3,558.7 µs` | `1,001.4 µs` | `3,975.5 µs` | `4,080.6 µs` | `4,104.7 µs` | **`4,105 µs`** | `100,000 µs` | **`0`** | **100.00%** |
| **E3-B** | **N1** — Acceso local con Wi-Fi + Telegram | `rfid_to_lock_us` | `30` | `112 µs` | `126.5 µs` | `8.6 µs` | `125.5 µs` | `141.6 µs` | `143.4 µs` | **`144 µs`** | `50,000 µs` | **`0`** | **100.00%** |
| **E3-B** | **S3** — Ruta `N1→S3→N2` **con Wi-Fi + HTTPS** | `s3_route_n1_to_n2_us` | `30` | `1,102 µs` | `3,353.2 µs` | `909.4 µs` | `3,732.5 µs` | `3,899.7 µs` | `4,104.6 µs` | **`4,149 µs`** | `100,000 µs` | **`0`** | **100.00%** |
| **E4-A** | **S3** — Telegram red primaria (`slot 0`, ráfaga con `HTTP 429`) | `tg_alert_e2e_us` | `17` | `552.1 ms` | `2,178.5 ms` | `2,669.8 ms` | **`665.6 ms`** | `8,640.3 ms` | `8,994.8 ms` | `9,083.4 ms` | `3,000 ms` | `3` | `82.35%` |
| **E4-B** | **S3** — Telegram hotspot (`slot 1`, `POCO X4 GT`, sin `429`) | `tg_alert_e2e_us` | `10` | `579.1 ms` | `1,402.2 ms` | `1,133.1 ms` | **`822.4 ms`** | `3,393.1 ms` | `3,920.8 ms` | `4,052.7 ms` | `3,000 ms` | `1` | `90.00%` |
| **E5** | **N3** — Jitter DHT11 RMT (`10 min`, `T=3,000 ms`) | `dht11_jitter_us` | `201` | `512 µs` | `518.0 µs` | **`1.9 µs`** | `519.0 µs` | `519.0 µs` | `519.0 µs` | **`519 µs`** | `50,000 µs` | **`0`** | **100.00%** |

### Análisis técnico clave (`E3`, `E4`, `E5` y `E6`)
- **Impacto de coexistencia Wi-Fi + BLE (`E3`):** al comparar `s3_route_n1_to_n2_us` sin Wi-Fi (`WCRT = 4,105 µs`) frente a Wi-Fi + tráfico HTTPS Telegram activo (`WCRT = 4,149 µs`), la variación en el peor caso fue de solo **`+1.1 %`** (`0 misses`, `~24×` por debajo de `D = 100 ms`), demostrando la efectividad del **Core Pinning** (Core 0 radio/coexistencia; Core 1 FSM/Telegram) y la topología **BLE GATT en estrella**.
- **Frontera Firm RT vs. Soft RT (`E1`, `E2`, `E3`, `E5` vs. `E4`):** todas las tareas críticas locales y de la estrella BLE cumplieron al **100.00 % (`0 misses`)**. En contraste, la ruta nube de Telegram (`E4`, clasificada como **Soft RT**) entregó una mediana de **`665.6 ms`** en red fija y **`822.4 ms`** en hotspot móvil, pero experimentó picos `> 3,000 ms` cuando la API externa aplicó *rate-limiting* de grupo (`HTTP 429`, 20 msg/min) o variabilidad celular.
- **Determinismo del periférico RMT (`E5`):** durante `10 min` continuos (`201` ciclos de `3,000 ms`, `uptime = 1,080 s`), `task_dht11_rmt` registró un jitter de **`518.0 ± 1.9 µs`** (`WCRT = 519 µs`, `~96×` bajo el límite de `50 ms`), validando la ausencia de bloqueos del planificador FreeRTOS.
- **Resiliencia offline y supervisión TWDT (`E6`):**
  - Con `WiFi=OFFLINE` (`failovers=2`), el Maestro S3 mantuvo **`BLE=3/3`** y completó `30/30` enrutamientos `N1 → S3 → N2` sin degradación (`uptime = 1,753 s`, `min_heap = 8,259,416 B` estable sin fugas; `N1 min_heap = 356,212 B`; `N2 min_heap = 366,168 B`; `N3 min_heap = 371,260 B`).
  - El **Task Watchdog Timer (`TWDT = 10 s`, `panic=on`)** permaneció activo en las 4 placas durante toda la jornada experimental (`~30 min` continuos) con `0` disparos inesperados y márgenes de stack libres ≥ 25 % en todas las tareas críticas (`task_alarm_fsm=3,180 B`, `task_rfid_poll=3,468 B`, `task_dht11_rmt=2,332 B`, `task_state_fsm_s3=1,552 B`, `task_tg_tx=4,332 B`, `task_tg_rx=4,340 B`, `task_wifi_sup=2,684 B`).

---

### Contraste de Hipótesis Experimentales (`H1–H5`)

| ID | Hipótesis planteada | Evidencia experimental (`E1–E6`) | Veredicto |
| :---: | :--- | :--- | :---: |
| **H1** | La activación por interrupción (`xTaskNotifyFromISR`) y prioridades fijas en FreeRTOS mantiene el WCRT local de alarma muy por debajo de `D = 10 ms`. | En **E1** (`N=50`), `pir_to_led_us` registró media `33.6 µs`, `σ = 2.2 µs` y **WCRT = `40 µs`** (`250×` de holgura, `0` misses). | **Validada** |
| **H2** | Aislar radio/coexistencia en **Core 0** y coordinación/TLS en **Core 1** sobre estrella BLE GATT evita degradación severa al operar Wi-Fi + HTTPS. | En **E3** (`N=30` por condición), `s3_route_n1_to_n2_us` pasó de `4,105 µs` (sin Wi-Fi) a `4,149 µs` (con Wi-Fi + Telegram): **`ΔWCRT = +1.1 %`**, `0` misses. | **Validada** |
| **H3** | Existe una frontera clara entre el dominio local (**Firm RT**, determinista) y la nube (**Soft RT**, sujeta a latencia WAN y *rate-limiting*). | El 100 % de las métricas locales (`E1`, `E2`, `E3`, `E5`) cerró con `0` misses; en **E4**, Telegram tuvo `p50 = 665.6 ms` (fija) y `822.4 ms` (hotspot), pero ráfagas provocaron `HTTP 429` (`WCRT = 9,083.4 ms`). | **Validada** |
| **H4** | Delegar el protocolo 1-wire del DHT11 al periférico hardware **RMT** evita bloqueos del planificador FreeRTOS y acota el *jitter* periódico. | En **E5** (`10 min`, `N=201`, `T=3,000 ms`), `dht11_jitter_us` fue **`518.0 ± 1.9 µs`** (`WCRT = 519 µs`, `~96×` bajo `D = 50 ms`, `0` misses). | **Validada** |
| **H5** | La estrella BLE mantiene autonomía local total sin Internet y el failover dual Wi-Fi recupera la salida a nube sin soltar enlaces BLE. | En **E2/E6**, `30/30` rutas `N1→S3→N2` operaron con `WiFi=OFFLINE`; el failover `slot 0 ↔ slot 1` conservó `BLE=3/3` y memoria estable sin disparos TWDT. | **Validada** |

---

### Análisis Ampliado de Resultados

1. **Márgenes de planificabilidad y dispersión (`p50` / `p95` / `p99` / `σ`):**
   - La relación entre `p50` y `WCRT` en las rutas críticas locales es extremadamente cerrada: en **E1** (`33.0 µs` vs `40 µs`, `σ = 2.2 µs`), en **E2/E3** para acceso N1 (`125.0 µs` vs `138–144 µs`, `σ ≤ 8.6 µs`) y en **E5** (`519.0 µs` vs `519 µs`, `σ = 1.9 µs`). Esto demuestra ausencia de *priority inversion* y de bloqueos no acotados en las tareas **Firm RT**.
   - En **E3**, la cola de enrutamiento `s3_route_n1_to_n2_us` presenta una desviación estándar mayor (`~909–1,001 µs`, rango `1.0–4.1 ms`) porque el tiempo de respuesta incluye el encolado hacia la tarea Host de NimBLE y la escritura GATT (`0xFF12`) alineada al intervalo de conexión BLE, manteniendo aun así `~24×` de margen respecto a `D = 100 ms`.
2. **Efecto de optimizaciones en tiempo de ejecución (`task_wifi_sup` y `HTTP 429`):**
   - Durante la validación inicial de Fase 4 se detectó que `task_wifi_sup` consumía `52 %` de CPU por un *busy-wait* sobre `WIFI_EVT_CONNECTED_BIT`; tras corregirlo, su consumo cayó a **`<1 %`** (`IDLE0 = 93%–96%`, `IDLE1 = 92%–98%`), confirmando la importancia de perfilar con `vTaskGetRunTimeStats()` en placa real.
   - En **E4-A**, la ráfaga de 20 ciclos seguidos (~60 mensajes en ~2 min) activó el límite de tasa de Telegram (`HTTP 429`), elevando `p95` a `8.64 s`. Esto justifica la arquitectura implementada: cola `TG_PRIO_URGENT` separada de `TG_PRIO_NORMAL`, supresión de mensajes intermedios redundantes y desacoplamiento total respecto a la seguridad local (`N1 → S3 → N2`).
3. **Gestión de memoria heterogénea (SRAM interna vs. Octal PSRAM):**
   - Al alojar los buffers TLS/mbedTLS, HTTP (`16 KB` RX / `2.5 KB` TX) y parser JSON en **Octal PSRAM** (`8,058 KB` libres de `8,192 KB`), el Gateway S3 conservó `93 KB` de SRAM interna libre (`min = 38 KB`) y todos los stacks críticos con holgura ≥ 25 %.
