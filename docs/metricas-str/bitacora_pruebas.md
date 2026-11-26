# Bitácora de Pruebas y Resultados Experimentales

Registro cronológico de validaciones en hardware real, mediciones de Sistemas en Tiempo Real (STR) y estado de avance por fase del proyecto.

---

## Resumen de Estado por Nodo y Fase

> **Fases 2 y 3 — Completadas:** los 3 nodos `ESP32-C6` individuales y la red **BLE GATT en estrella** (`ESP32-S3` Maestro ↔ `3× ESP32-C6` Periféricos) quedaron validados en hardware real con métricas STR dentro de deadline (`misses=0`).

| Componente / Fase | Placa / Hardware | Estado | Última verificación |
| :--- | :--- | :--- | :--- |
| **Fase 2 — Nodo 2 (`nodo2_seguridad_c6`)** | `ESP32-C6-DevKitC-1` + PIR `HC-SR501` | **Validado en hardware** | Compilación + corrida continua `1022 s` |
| **Fase 2 — Nodo 1 (`nodo1_acceso_c6`)** | `ESP32-C6-DevKitC-1` + `RFID-RC522` | **Validado en hardware** | Compilación + pruebas SPI, NVS y `BOOT` |
| **Fase 2 — Nodo 3 (`nodo3_clima_c6`)** | `ESP32-C6 SuperMini` + `DHT11` | **Validado en hardware** | Compilación + corrida inicial `186 s` + corrida post-fix `75 s` sin errores |
| **Fase 3 — Red BLE GATT en Estrella** | `3× ESP32-C6` + `ESP32-S3 N16R8 UNO` | **Validado en hardware** | Corrida continua `1782 s` (~30 min), reconexión `3/3` y ruta `N1→S3→N2` |
| **Fase 4 — Coexistencia + Telegram** | `ESP32-S3 N16R8 UNO` + `3× ESP32-C6` | **Validado en hardware** | Octal PSRAM, `esp_coex`, failover dual Wi-Fi (`slot 0 ↔ slot 1`), `/panel` y comandos Telegram |
| **Fase 5 — Integral, `/rfid` y Offline** | Sistema completo (`3× C6 + S3 + Telegram`) | **Validado en hardware** | Flujo completo, 3 fallos RFID, `/rfid` NVS, `/umbral`, sobresalto térmico, failover y reconexión `3/3` |

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

## Próximas Entradas (Fase 6)

- **Fase 6:** resultados consolidados de la matriz experimental `E1–E6` con scripts de análisis en `tools/`.
