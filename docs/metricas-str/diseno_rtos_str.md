# Diseño Formal de Sistemas en Tiempo Real (STR) sobre FreeRTOS (Fase 1)

Este documento constituye la base técnica de ingeniería en **Sistemas en Tiempo Real (STR)** para el proyecto, incluyendo la sustentación formal del RTOS elegido, el modelo de tareas y prioridades, las políticas de memoria/Watchdog y el plan experimental de métricas por software.

---

## 1. Sustentación formal para STR: FreeRTOS (ESP-IDF SMP) vs. Zephyr RTOS

En la asignatura de Sistemas en Tiempo Real es fundamental justificar por qué la arquitectura utiliza **ESP-IDF puro + FreeRTOS** en lugar de **Zephyr RTOS**:

### Argumentos técnicos desde la perspectiva de Tiempo Real
1. **Planificación preventiva determinista de prioridades fijas (*Fixed-Priority Preemptive Scheduling*):** FreeRTOS implementa exactamente 25 niveles de prioridad (configurables vía `configMAX_PRIORITIES`) con despacho $O(1)$ mediante mapa de bits por hardware/arquitectura. Esto permite aplicar directamente asignación de prioridades tipo **Rate Monotonic (RM)** y por **criticidad temporal** sin capas intermedias de abstracción.
2. **Prevención de inversión de prioridad:** el uso de `xSemaphoreCreateMutex()` activa nativamente el protocolo de **Herencia de Prioridad (*Priority Inheritance*)**, garantizando acotar el tiempo de bloqueo cuando una tarea de alta prioridad y otra de baja comparten recursos (por ejemplo, el bus SPI o la tabla de estado global).
3. **Notificaciones directas de latencia mínima (`xTaskNotifyFromISR`):** para el disparo del sensor PIR en Nodo 2, la ISR despierta la tarea de alarma mediante notificación directa al TCB de la tarea, evitando la copia en memoria y el overhead de una cola clásica (~45 % más rápido según documentación oficial de FreeRTOS).
4. **Aislamiento físico por núcleos (*Core Pinning* SMP en ESP32-S3):** mediante `xTaskCreatePinnedToCore()`, el Gateway ESP32-S3 confina las pilas asíncronas de radio (**NimBLE GATT + Wi-Fi + LwIP + `esp_coex`**) al **Core 0**, aislando el **Core 1** para la máquina de estados global, la cola prioritaria de alertas y las transacciones bloqueantes TLS/HTTPS con Telegram.
5. **Determinismo de periféricos críticos y coexistencia probada:**
   - En el Nodo 3, el **DHT11** exige pulsos del orden de **20–80 µs**. Usar *bit-banging* por CPU deshabilitando interrupciones destruye el determinismo de FreeRTOS; en ESP-IDF se utiliza el **periférico hardware RMT**, que captura los flancos por memoria dedicada sin bloquear la CPU ni alterar el planificador.
   - En el ESP32-S3, la coexistencia **Wi-Fi + BLE (NimBLE)** depende directamente del árbitro de hardware/software `esp_coex` y del soporte maduro de **Octal PSRAM (`N16R8`)**, ambos integrados y validados nativamente por Espressif en ESP-IDF v6.1.1. Usar **BLE GATT en estrella sobre NimBLE** en lugar de BLE Mesh elimina las ventanas continuas de escaneo/reenvío *Relay*, reduciendo la contención con el tráfico Wi-Fi HTTPS.

### Comparativa objetiva FreeRTOS (ESP-IDF) vs. Zephyr RTOS

| Criterio STR / Ingeniería | FreeRTOS (ESP-IDF v6.1.1 puro) | Zephyr RTOS |
| :--- | :--- | :--- |
| **Determinismo de radio dual (BLE NimBLE + Wi-Fi)** | **Óptimo:** árbitro oficial `esp_coex` sintonizado por el fabricante del silicio. | **Riesgo alto:** soporte BLE + Wi-Fi simultáneo en ESP32-S3 aún requiere ajustes complejos sobre blobs binarios. |
| **Periféricos críticos (RMT + Octal PSRAM + TWDT)** | **Nativo:** APIs estables de RMT, SPI DMA, PSRAM Octal y Task Watchdog por tarea. | Requiere escribir y depurar *Devicetree overlays* y drivers no siempre completos para C6/S3. |
| **Análisis formal e instrumentación STR** | Primitivas directas: `vTaskGetRunTimeStats`, `uxTaskGetStackHighWaterMark`, `esp_timer_get_time()`. | Excelente subsistema de tracing, pero añade sobrecarga conceptual de Devicetree/West ajena al objetivo RTOS. |
| **Portabilidad multi-fabricante** | Baja (orientado a ESP32). | **Excelente** (ventaja decisiva **solo** si el proyecto combinara MCUs de distintos fabricantes como Nordic + STM32 + ESP32). |

**Conclusión:** siendo una plataforma homogénea 100 % Espressif (`3× ESP32-C6` + `1× ESP32-S3`), **ESP-IDF puro + FreeRTOS** ofrece mayor determinismo real, menor latencia de integración y total transparencia para el análisis de planificabilidad en STR.

---

## 2. Tablas formales de tareas RTOS, prioridades y Deadlines

> **Convención de prioridades ESP-IDF:** rango típico de aplicación `1 .. 24` (`configMAX_PRIORITIES = 25`), donde **mayor número = mayor prioridad**. Las tareas internas del sistema (`esp_timer`, controlador BLE, `wifi`, `btController`) operan entre prioridades `18..23`. Las tareas de aplicación se diseñan en rango `2..17` para nunca hambrear a los controladores de radio.

### 2.1 Nodo 1 — Acceso (`ESP32-C6-DevKitC-1`)

| Tarea / ISR | Tipo | Período `T` / `T_min` | Deadline `D` | Criticidad | Prio | Stack (B) | IPC / Primitivas | Condición de *Deadline Miss* |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `task_rfid_poll` | Periódica | `T = 100 ms` | `D = 50 ms` (detección → LED cerradura) | **Firm RT** | **12** | 4096 | `SPI2` + `Mutex` NVS + notificación GATT (`0xFF11`) | Tiempo `(fin validación + LED) - (lectura UID)` > 50 ms |
| `task_ble_n1` | Esporádica | `T_min = 50 ms` | `D = 150 ms` (cmd `/cerradura` → LED) | **Firm RT** | **10** | 4096 | `xQueue` RX/TX + `OP_PROTO_ACK` / `OP_RFID_MGMT_ACK` | Tiempo desde `rx_cb` hasta actualización LED > 150 ms |
| `task_boot_btn` | Esporádica | `T = 100 ms` | `D = 200 ms` | Soft RT | **4** | 2048 | Polling antirrebote GPIO 9 / `xTimer` | No aplica (supervisión usuario) |
| `task_hb_n1` | Periódica | `T = 15 s` | `D = 1000 ms` | Soft RT | **3** | 3072 | `xTimer` + TWDT feed | Retraso envío heartbeat > 1 s |

### 2.2 Nodo 2 — Seguridad e Intrusión (`ESP32-C6-DevKitC-1`)

| Tarea / ISR | Tipo | Período `T` / `T_min` | Deadline `D` | Criticidad | Prio | Stack (B) | IPC / Primitivas | Condición de *Deadline Miss* |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `pir_gpio_isr` | **ISR** | Esporádica (`T_min ≥ 500 ms`) | `D < 100 µs` | **Hard/Firm RT** | ISR | — | `xTaskNotifyFromISR` + captura `t_isr = esp_timer_get_time()` | Latencia de despacho ISR > 100 µs |
| `task_alarm_fsm` | Esporádica | Evento PIR / BLE GATT / `xTimer` | **`D = 10 ms`** (ISR → LED encendido + enqueue TX) | **Firm RT** | **16** | 4096 | `ulTaskNotifyTake` + `xTimer` (patrón LED 100/150 ms) + `Mutex` estado | `(t_led_on - t_isr) > 10 ms` |
| `task_ble_n2` | Esporádica | `T_min = 50 ms` | `D = 150 ms` | **Firm RT** | **11** | 4096 | `xQueue` urgente de alertas + `OP_ALARM_EVENT` | Envío de disparo no iniciado antes de 150 ms |
| `task_hb_n2` | Periódica | `T = 15 s` | `D = 1000 ms` | Soft RT | **3** | 3072 | `xTimer` + TWDT feed | Retraso envío heartbeat > 1 s |

### 2.3 Nodo 3 — Climatización y Alertas Térmicas (`ESP32-C6 SuperMini`)

| Tarea / ISR | Tipo | Período `T` / `T_min` | Deadline `D` | Criticidad | Prio | Stack (B) | IPC / Primitivas | Condición de *Deadline Miss* |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `task_temp_alert` | Esporádica | Evento lectura / cmd umbral | `D = 50 ms` (evaluación sobresalto + RGB/Notify) | **Firm RT** | **11** | 3072 | `xQueue` lecturas + `Mutex` NVS umbral | Tiempo desde recepción de lectura hasta evaluación > 50 ms |
| `task_dht11_rmt` | Periódica | `T = 3000 ms` | `D = 500 ms` (jitter máx. permitido `±50 ms`) | Soft RT | **6** | 3072 | Canal **RMT** hardware (sin busy-wait) + `vTaskDelayUntil` | `|t_real - t_esperado| > 50 ms` (jitter) o ciclo > 500 ms |
| `task_hb_n3` | Periódica | `T = 15 s` | `D = 1000 ms` | Soft RT | **3** | 3072 | `xTimer` + TWDT feed | Retraso envío heartbeat > 1 s |

### 2.4 Gateway / Maestro BLE GATT (`ESP32-S3 N16R8 UNO` — Dual Core)

| Tarea / Sistema | Núcleo | Tipo | Período / Evento | Deadline `D` | Criticidad | Prio | Stack (B) | IPC / Primitivas | Condición de *Deadline Miss* |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| NimBLE Host + Wi-Fi + LwIP + `esp_coex` | **Core 0** | Sistema | Asíncrono / Radio | Según stack | Sistema | 18–23 | — | Colas internas + árbitro `esp_coex` | Pérdida de slot BLE/Wi-Fi |
| `nimble_host` (`ble_central_star`) | **Core 0** | Esporádica | Eventos Notify/Write GATT | `D = 100 ms` | **Firm RT** | **14** | 5120 | Callbacks NimBLE ↔ `xQueue` inter-núcleo hacia Core 1 | Encolado hacia Core 1 > 100 ms |
| `task_state_fsm_s3` | **Core 1** | Esporádica | Eventos BLE GATT + Telegram | `D = 100 ms` | **Firm RT** | **12** | 4096 | `xQueue` unificada + `Mutex` tabla de estado global + timeout offline (45 s) | Actualización de estado > 100 ms |
| `task_tg_tx` | **Core 1** | Esporádica | Cola `PRIO_URGENT` / `PRIO_NORMAL` | `D_urgent = 3000 ms` | Soft RT | **8** | 8192 | `xQueue` doble (urgente primero) + `esp_http_client` (`keep-alive`) | Alerta urgente Telegram > 3000 ms (con Wi-Fi activo) |
| `task_tg_rx` | **Core 1** | Periódica / Blocking | *Long Polling* `getUpdates` (15 s) | `D = 20 s` | Soft RT | **5** | 8192 | `esp_http_client` + parser JSON en C puro (buffers en Octal PSRAM) | Bloqueo de polling sin recuperación > 25 s |
| `task_wifi_sup` | **Core 1** | Periódica / Evento | Monitoreo / Failover Wi-Fi | `D = 20 s` (conmutación red) | Soft RT | **4** | 4096 | `EventGroup` Wi-Fi + rotación red primaria/secundaria | No intenta failover tras pérdida |

---

## 3. Políticas de Memoria, Watchdog y Supervisión

1. **Gestión de memoria dinámica controlada (Heap):**
   - Todas las tareas y colas se crean durante la fase de inicialización (`app_main`) sin asignaciones/liberaciones continuas en los lazos críticos.
   - En cada ciclo de métricas se registra **`uxTaskGetStackHighWaterMark()`** por tarea y **`esp_get_minimum_free_heap_size()`** para demostrar ausencia de fugas y verificar margen mínimo ≥ 25 % en cada stack.
   - En el **ESP32-S3 N16R8**, todos los stacks de tareas críticas permanecen en **SRAM interna** (`MALLOC_CAP_INTERNAL`), destinando la **Octal PSRAM** (`MALLOC_CAP_SPIRAM`) a mbedTLS, certificados y payloads JSON de Telegram.
2. **Task Watchdog Timer (TWDT):**
   - Habilitado en las 4 placas con timeout de **10 segundos** (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=10`).
   - Las tareas permanentes críticas alimentan explícitamente `esp_task_wdt_reset()` en su ciclo; un bloqueo infinito reinicia automáticamente el nodo afectado y su enlace BLE GATT en estrella se reconecta automáticamente.
3. **Detección de nodos offline (*Heartbeats*):**
   - Cada C6 notifica `OP_HEARTBEAT` cada **15 segundos** al Maestro S3 por GATT (`0xFF11`).
   - Si el Gateway S3 supera **45 segundos** (3 periodos perdidos) sin recibir señal de un nodo C6, lo marca como `OFFLINE` en `/estado` y notifica al grupo de Telegram (y vuelve a notificar al reconectarse).

---

## 4. Instrumentación 100 % por Software y Plan Experimental de Métricas

No se reservan pines GPIO para osciloscopio ni analizador lógico; toda medición usa el reloj monotónico hardware de **1 µs de resolución** (`esp_timer_get_time()`), más `vTaskGetRunTimeStats()` y `uxTaskGetStackHighWaterMark()`.

### Formato estándar de salida para análisis automatizado (`tools/`)

Las placas imprimen métricas parseables por puerto serie siguiendo una sintaxis uniforme para que un futuro script en `tools/` genere automáticamente promedios, percentiles (p50, p95, p99, máximo/WCRT) y tablas:

```text
[STR_METRIC] node=N2 metric=pir_to_led_us value_us=420 deadline_us=10000 miss=0
[STR_METRIC] node=N3 metric=dht11_jitter_us value_us=3120 deadline_us=50000 miss=0
[STR_METRIC] node=S3 metric=s3_route_n1_to_n2_us value_us=3732 deadline_us=100000 miss=0
[STR_METRIC] node=S3 metric=tg_alert_e2e_us value_us=1420000 deadline_us=3000000 miss=0
```

### Datos reportados en vivo por el comando `/metricas` en Telegram
Al invocar `/metricas` (o pulsar el botón en `/panel`), el Gateway S3 consulta (`OP_STATUS_REQ` con flag de métricas) y consolida:
- `max_crit_latency_us` y `deadline_misses` acumulados de Nodo 1, Nodo 2 y Nodo 3.
- `avg_jitter_us` del muestreo periódico DHT11 (Nodo 3).
- Estado de enlace BLE (`RSSI`, `uptime_s`) y `min_stack_bytes` reportados por cada C6.
- `% CPU` por tarea/núcleo (`vTaskGetRunTimeStats`), `min_stack_bytes` más ajustado y memoria libre (`SRAM interna` y `Octal PSRAM` en el S3).

### Matriz de experimentos para el informe final de IoT + STR

| # | Experimento | Qué se mide | Condición de prueba | Propósito académico |
| :--- | :--- | :--- | :--- | :--- |
| **E1** | **WCRT local en Nodo 2** | `t_led_on - t_pir_isr` (µs) | 50 disparos PIR con alarma `ARMADA` | Validar cumplimiento estricto de `D = 10 ms` (Firm RT) |
| **E2** | **Coordinación en estrella (`N1 → S3 → N2`)** | Tiempo desde lectura RFID en N1 hasta cambio de LED alarma en N2 | Realizar **sin Wi-Fi / sin Internet** (solo estrella BLE) | Demostrar funcionamiento estilo carro offline independiente de Internet |
| **E3** | **Latencia BLE GATT + Coexistencia** | Latencia `C6 → S3` (Notify `0xFF11`) y `S3 → C6` (Write `0xFF12`) | Repetir **sin Wi-Fi/Telegram** y **con tráfico HTTPS Telegram activo** | Cuantificar efecto de la radio compartida (`esp_coex`) en S3 |
| **E4** | **Latencia Extremo a Extremo (Telegram)** | Tiempo `PIR → mensaje en grupo` y `/cerradura abrir → LED N1` | 20 repeticiones en red primaria y en hotspot de respaldo | Evaluar rendimiento QoS IoT completo (Soft RT) |
| **E5** | **Jitter de tarea periódica (DHT11 RMT)** | `|T_real - 3000 ms|` y tiempo de lectura RMT | Registro continuo durante 10 minutos | Demostrar que RMT evita bloqueos del planificador FreeRTOS |
| **E6** | **Resiliencia Offline y Watchdog (TWDT)** | Comportamiento al desconectar router Wi-Fi + inyección de bucle infinito (`while(1)`) | Medir recuperación NVS y continuidad local | Validar tolerancia a fallos IoT y recuperación automática TWDT |
