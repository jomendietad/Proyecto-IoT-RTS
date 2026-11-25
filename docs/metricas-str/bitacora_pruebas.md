# Bitácora de Pruebas y Resultados Experimentales

Registro cronológico de validaciones en hardware real, mediciones de Sistemas en Tiempo Real (STR) y estado de avance por fase del proyecto.

---

## Resumen de Estado por Nodo y Fase

| Componente / Fase | Placa / Hardware | Estado | Última verificación |
| :--- | :--- | :--- | :--- |
| **Fase 2 — Nodo 2 (`nodo2_seguridad_c6`)** | `ESP32-C6-DevKitC-1` + PIR `HC-SR501` | **Validado en hardware** | Compilación + corrida continua `1022 s` |
| **Fase 2 — Nodo 1 (`nodo1_acceso_c6`)** | `ESP32-C6-DevKitC-1` + `RFID-RC522` | **Validado en hardware** | Compilación + pruebas SPI, NVS y `BOOT` |
| **Fase 2 — Nodo 3 (`nodo3_clima_c6`)** | `ESP32-C6 SuperMini` + `DHT11` | **Compilado (pendiente prueba física)** | `0x3a220` bytes, 0 warnings (`esp32c6`) |
| **Fase 3 — Red BLE Mesh** | `3× ESP32-C6` + `ESP32-S3 N16R8 UNO` | Pendiente | — |
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

### 3. Nodo 3 — Climatización (`nodo3_clima_c6`)

- **Estado:** firmware implementado y compilado limpiamente para `esp32c6` (`0x3a220` bytes, 77 % libre, 0 warnings). **Pendiente de pruebas físicas en placa** (`ESP32-C6 SuperMini` + `DHT11` + LED PWM `LEDC`).
- **Puntos a registrar cuando se pruebe en placa:**
  - Lectura real del `DHT11` vía `RMT RX` (`GPIO 2`) sin errores de CRC/timeout.
  - Jitter periódico `dht11_jitter_us` (deadline `50,000 µs` sobre `T = 3,000 ms`).
  - Transiciones `OFF / BAJA / MEDIA / ALTA` con histéresis de `1.0 °C` y control manual `fan`.

---

## Próximas Entradas (Fases 3 a 6)

- **Fase 3:** registro de aprovisionamiento automático PB-ADV, interacción directa `0xC001` (Nodo 1 ↔ Nodo 2) y pruebas multi-salto (`/mesh demo_hop`).
- **Fase 4–5:** métricas de coexistencia Wi-Fi + BLE Mesh en ESP32-S3, tiempos de Telegram y prueba *offline*.
- **Fase 6:** resultados consolidados de la matriz experimental `E1–E6`.
