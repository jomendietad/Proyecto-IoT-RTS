# Protocolo BLE GATT en Estrella y Tramas Binarias

Este documento define el protocolo de comunicaciones **Bluetooth Low Energy en topología estrella (BLE GATT sobre NimBLE)** compartido por las 4 placas mediante `components/common_types/` y `components/ble_protocol/`.

---

## 1. Arquitectura en Estrella (NimBLE)

- **Maestro / Central (`ESP32-S3 N16R8 UNO`):** mantiene hasta **3 conexiones BLE GATT simultáneas** hacia los 3 periféricos `ESP32-C6`, actúa como coordinador central de eventos (incluyendo el puente `Nodo 1 → S3 → Nodo 2`) y Border Router Wi-Fi/Telegram.
- **Periféricos (`3× ESP32-C6`):** operan en modo **GATT Server**, anuncian su nombre fijo mediante Advertising BLE y exponen un único servicio de aplicación ligero. No realizan *Relay* entre sí, lo que reduce drásticamente la contención de radio y simplifica el mantenimiento del enlace.

### Identificación BLE GATT de los periféricos

| Rol | Placa | Nombre Advertising (`GAP`) | Dirección lógica | Periféricos físicos |
| :--- | :--- | :--- | :--- | :--- |
| **Maestro (Central)** | `ESP32-S3 N16R8 UNO` | `IOT-S3-MAESTRO` | `0x0001` | Sin sensores externos |
| **Nodo 1 (Periférico)** | `ESP32-C6-DevKitC-1` | **`IOT-N1-ACCESO`** | `0x0010` | RFID-RC522 + LED Cerradura |
| **Nodo 2 (Periférico)** | `ESP32-C6-DevKitC-1` | **`IOT-N2-SEGURIDAD`** | `0x0020` | PIR HC-SR501 + LED Alarma |
| **Nodo 3 (Periférico)** | `ESP32-C6 SuperMini` | **`IOT-N3-CLIMA`** | `0x0030` | DHT11 (alertas por sobresalto) |

---

## 2. Perfil GATT y Reconexión Automática

Cada nodo ESP32-C6 expone el mismo servicio GATT (`16-bit UUID`):

| Elemento GATT | UUID | Propiedades | Sentido / Uso |
| :--- | :--- | :--- | :--- |
| **Servicio Principal** | **`0xFF10`** | Primary Service | Servicio de telemetría y control IoT-STR |
| **Característica `EVT_TX`** | **`0xFF11`** | `Read` + **`Notify`** | **C6 → Maestro S3:** envío asíncrono de eventos, alertas, heartbeats y métricas |
| **Característica `CMD_RX`** | **`0xFF12`** | **`Write`** (con respuesta ATT) | **Maestro S3 → C6:** envío fiable de comandos (armar/desarmar, cerradura, `/umbral`, `/rfid`, `/estado`) |

### Formato de trama sobre GATT (`[opcode | payload]`)
Cada notificación (`0xFF11`) o escritura (`0xFF12`) transporta **1 byte de `opcode` (`0xC0..0xCE`)** seguido inmediatamente del **payload binario empaquetado** definido en la Sección 3.

### Ciclo de conexión y reconexión automática del Maestro S3
```text
  Periférico C6 (GATT Server)                      Maestro ESP32-S3 (Central NimBLE)
             │                                                 │
             │── ADV_IND ("IOT-N1-ACCESO" / N2 / N3) ─────────►│ Filtra nombre del proyecto
             │◄──────────── BLE_GAP_CONNECT ───────────────────│ Asigna conn_handle por rol
             │◄════════════ Descubrimiento Servicio 0xFF10 ═══►│ Localiza handles 0xFF11 / 0xFF12
             │◄── Write CCCD 0xFF11 = 0x0001 (Enable Notify) ──│ Suscripción automática
             │                                                 │
             │═══ Notify 0xFF11: [opcode | payload] ══════════►│ Uplink eventos / heartbeats
             │◄══ Write  0xFF12: [opcode | payload] ═══════════│ Downlink comandos / enrutamiento N1→N2
             │                                                 │
         [Si cae enlace]                                  [Si falta algún periférico]
             │── Reinicia Advertising inmediato                │── Reanuda escaneo pasivo en Core 0
```

- Todas las placas mantienen **NVS persistente** para credenciales/configuración local (`rfid_store` en N1, `t_high_c` en N3).
- El botón **`BOOT` (5 s)** (`GPIO 9` en C6, `GPIO 0` en S3) borra la NVS de aplicación y reinicia la placa limpiamente.

---

## 3. Catálogo de Opcodes y Payloads Binarios (`components/ble_protocol/`)

Todos los campos multibyte viajan en **little-endian** y están validados en compilación con `_Static_assert`.

| Opcode | Nombre simbólico | Sentido | Propósito |
| :--- | :--- | :--- | :--- |
| **`0xC0`** | `OP_ALARM_SET` | N1 → S3 / S3 → N2 | Armar o desarmar alarma (desde Telegram o RFID válido vía Maestro S3) |
| **`0xC1`** | `OP_ALARM_EVENT` | N2 → S3 | Reporte de transición (`DESARMADA`, `ARMADA`, `DISPARADA`) + causa y latencia |
| **`0xC2`** | `OP_RFID_AUTH_FAIL` | N1 → S3 → N2 | Notifica intento de tarjeta RFID no autorizada (incrementa contador 1..3 en N2) |
| **`0xC3`** | `OP_LOCK_SET` | S3 → N1 | Comando remoto `/cerradura abrir` o `/cerradura cerrar` |
| **`0xC4`** | `OP_LOCK_STATUS` | N1 → S3 | Evento de cambio en cerradura (abierta/cerrada, fuente RFID/remoto, UID) |
| **`0xC5`** | `OP_RFID_MGMT` | S3 → N1 | Operaciones `/rfid list`, `/rfid add <UID>`, `/rfid del <UID>` |
| **`0xC6`** | `OP_RFID_MGMT_ACK` | N1 → S3 | Respuesta con resultado y lista de UIDs almacenados en NVS de N1 |
| **`0xC7`** | `OP_CLIMA_TELEMETRY` | N3 → S3 | Temperatura, cambio `ΔT`, humedad, estado de alerta térmica y umbral `T_high` |
| **`0xC9`** | `OP_THRESHOLD_SET` | S3 → N3 | Comando `/umbral <°C>` para ajustar `T_high` (`15..50 °C`) |
| **`0xCA`** | `OP_HEARTBEAT` | C6 → S3 | Salud del nodo cada 15 s (`uptime`, `free_heap`, `deadline_misses`, flags) |
| **`0xCB`** | `OP_STATUS_REQ` | S3 → C6 | Solicita reporte inmediato de estado o métricas STR (`/estado`, `/metricas`) |
| **`0xCC`** | `OP_METRICS_REPORT` | C6 → S3 | Estadísticas STR (`max_crit_latency_us`, `avg_jitter_us`, `min_stack_bytes`, `%CPU`) |
| **`0xCE`** | `OP_PROTO_ACK` | Receptor → Emisor | Confirmación lógica opcional de aplicación (`ack_opcode`, `seq_num`, `status`) |

---

### Estructura binaria (Byte-by-Byte Payload Layout)

#### 1. Seguridad y Alarma (`0xC0` – `0xC2`)

- **`OP_ALARM_SET` (3 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `target_state` (`0 = DESARMADA`, `1 = ARMADA`)
  - `byte[2]`: `source` (`1 = TELEGRAM`, `2 = BLE_SRC_RFID_NODO1`)
- **`OP_ALARM_EVENT` (9 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `new_state` (`0 = DESARMADA`, `1 = ARMADA`, `2 = DISPARADA`)
  - `byte[2]`: `reason` (`0 = CMD_TG`, `1 = RFID_OPEN`, `2 = RFID_CLOSE`, `3 = PIR_MOTION`, `4 = RFID_3_FAILS`)
  - `byte[3..6]`: `local_latency_us` (`uint32_t`, latencia medida desde ISR/cmd hasta LED en Nodo 2)
  - `byte[7]`: `fail_counter` (`uint8_t`, 0..3)
  - `byte[8]`: `flags` (`bit0 = pir_active`)
- **`OP_RFID_AUTH_FAIL` (10 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `uid_len` (`uint8_t`, 4 o 7)
  - `byte[2..8]`: `uid[7]` (`uint8_t[7]`, rellenado con `0x00`)
  - `byte[9]`: `local_attempt_hint` (`uint8_t`)

#### 2. Acceso y Gestión RFID (`0xC3` – `0xC6`)

- **`OP_LOCK_SET` (2 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `lock_cmd` (`0 = CERRAR`, `1 = ABRIR`)
- **`OP_LOCK_STATUS` (11 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `lock_state` (`0 = CERRADA`, `1 = ABIERTA`)
  - `byte[2]`: `source` (`0 = TELEGRAM`, `1 = RFID_VALID`)
  - `byte[3]`: `uid_len` (`uint8_t`)
  - `byte[4..10]`: `uid[7]` (`uint8_t[7]`)
- **`OP_RFID_MGMT` (10 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `action` (`0 = LIST`, `1 = ADD`, `2 = DEL`)
  - `byte[2]`: `uid_len` (`uint8_t`)
  - `byte[3..9]`: `uid[7]` (`uint8_t[7]`)
- **`OP_RFID_MGMT_ACK` (variable):**
  - `seq_num`, `action`, `status`, `count` y lista de tarjetas almacenadas en NVS del Nodo 1.

#### 3. Alertas Térmicas (`0xC7`, `0xC9`)

- **`OP_CLIMA_TELEMETRY` (8 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1..2]`: `temp_deci_c` (`int16_t`, ej. `305` = `30.5 °C`)
  - `byte[3..4]`: `delta_deci_c` (`int16_t`, cambio respecto a lectura previa, ej. `+23` = `+2.3 °C`)
  - `byte[5]`: `humidity_pct` (`uint8_t`, `0..100 %`)
  - `byte[6]`: `alert_state` (`iot_temp_alert_t`: `0 = NORMAL`, `1 = HIGH`, `2 = SPIKE`, `3 = BOTH`)
  - `byte[7]`: `threshold_high_c` (`uint8_t`, `15..50 °C`, defecto `30 °C`)
- **`OP_THRESHOLD_SET` (2 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `new_t_high_c` (`uint8_t`, rango válido `15..50 °C`)

#### 4. Sistema y Métricas (`0xCA` – `0xCE`)

- **`OP_HEARTBEAT` (10 bytes):** `node_role`, `uptime_s`, `free_heap_kb`, `deadline_misses`, `flags` (`bit0 = ble_connected`, `bit1 = sensor_ok`).
- **`OP_STATUS_REQ` (1 byte):** `req_flags` (`0x01 = state`, `0x02 = str_metrics`, `0x03 = both`).
- **`OP_METRICS_REPORT` (15 bytes):** `node_role`, `max_crit_latency_us`, `avg_jitter_us`, `deadline_misses`, `min_stack_free_bytes`, `cpu_busy_pct`, `last_rx_hops` (`0` en enlace directo estrella).
- **`OP_PROTO_ACK` (3 bytes):** `ack_opcode`, `seq_num`, `status`.

---

## 4. Confiabilidad e Idempotencia

1. **Fiabilidad nativa en capa GATT:** los comandos críticos (`S3 → C6`) usan *Write with Response* en `0xFF12` (confirmación ATT garantizada por el controlador BLE) y las notificaciones `0xFF11` viajan sobre canales L2CAP orientados a conexión con CRC y retransmisión automática en capa de enlace.
2. **Idempotencia por `seq_num`:** mediante `ble_dedup_check()`, cada receptor ignora duplicados recientes por pareja `(src_addr, opcode)` para impedir ejecuciones dobles ante reconexiones.
