# Contrato de Red BLE Mesh y Protocolo de Mensajes (Fase 1)

Este documento define el protocolo de comunicaciones sobre **ESP-BLE-MESH** compartido por las 4 placas a través de los componentes `components/common_types/` y `components/mesh_protocol/`.

---

## 1. Arquitectura de red y direccionamiento

- **Rol del ESP32-S3 (`0x0001`):** **Provisioner** embebido, cliente/servidor del *Vendor Model* y Border Router hacia Wi-Fi/Telegram. Configurado explícitamente con **Relay DESHABILITADO (*NO-Relay*)** para no gastar ventanas de su radio compartida reenviando tramas entre terceros.
- **Rol de las 3 ESP32-C6 (`0x0010`, `0x0020`, `0x0030`):** Nodos de campo con **Relay HABILITADO (*Relay ON*)**, permitiendo cobertura multi-salto y comunicación directa **C6 ↔ C6** incluso si el Gateway S3 está temporalmente apagado o sin Internet.

### Tabla de direcciones Unicast y Grupos

| Tipo | Dirección | Elemento / Nombre lógico | Rol en la malla |
| :--- | :--- | :--- | :--- |
| Unicast | **`0x0001`** | Gateway `ESP32-S3 N16R8 UNO` | Provisioner, Coordinador FSM, Bridge Wi-Fi/Telegram (*NO-Relay*) |
| Unicast | **`0x0010`** | Nodo 1 `ESP32-C6` — Acceso | RFID-RC522 + LED Cerradura (*Relay ON*) |
| Unicast | **`0x0020`** | Nodo 2 `ESP32-C6` — Seguridad | PIR HC-SR501 + LED Alarma (*Relay ON*) |
| Unicast | **`0x0030`** | Nodo 3 `ESP32-C6` — Clima | DHT11 (RMT) + LED Ventilador PWM (*Relay ON*) |
| Grupo | **`0xC000`** | `GRP_SYSTEM` (Sistema / Broadcast) | *Heartbeats*, consulta de estado global, métricas y control `demo_hop` |
| Grupo | **`0xC001`** | `GRP_SECURITY` (Seguridad / Alarma) | Desarme/armado C6 ↔ C6 desde RFID, disparos PIR y comandos de alarma |
| Grupo | **`0xC002`** | `GRP_ACCESS` (Acceso) | Eventos RFID/cerradura, apertura/cierre remoto y gestión `/rfid` |
| Grupo | **`0xC003`** | `GRP_CLIMA` (Climatización) | Telemetría DHT11, control de ventilador y cambio de umbral |

### Matriz de Publicación / Suscripción por nodo

| Nodo | Publica en | Suscrito a |
| :--- | :--- | :--- |
| **Gateway S3 (`0x0001`)** | `0xC000`, `0xC001`, `0xC002`, `0xC003` (o Unicast directo) | `0xC000`, `0xC001`, `0xC002`, `0xC003` |
| **Nodo 1 Acceso (`0x0010`)** | `0xC000`, `0xC001` (armado/desarme por RFID), `0xC002` | `0xC000`, `0xC002` |
| **Nodo 2 Seguridad (`0x0020`)** | `0xC000`, `0xC001` | `0xC000`, `0xC001` |
| **Nodo 3 Clima (`0x0030`)** | `0xC000`, `0xC003` | `0xC000`, `0xC003` |

---

## 2. Aprovisionamiento Automático (PB-ADV)

El sistema no depende de una aplicación móvil externa (*nRF Mesh*). Al encender, el **ESP32-S3** busca beacons BLE Mesh no aprovisionados mediante el portador **PB-ADV** y configura automáticamente los nodos cuyo **Device UUID** pertenezca al proyecto.

### Formato exacto del Device UUID (16 bytes)

| Bytes | Campo | Valor | Descripción |
| :--- | :--- | :--- | :--- |
| `0..1` | `project_magic` | `0xAA, 0x55` | Prefijo identificador del proyecto (el S3 ignora dispositivos ajenos alrededor) |
| `2` | `proto_ver` | `0x01` | Versión de protocolo de malla |
| `3` | `node_role` | `0x01` / `0x02` / `0x03` | `0x01` = Nodo 1 Acceso, `0x02` = Nodo 2 Seguridad, `0x03` = Nodo 3 Clima |
| `4..9` | `mac_ble` | `6 bytes` | Dirección MAC Bluetooth de fábrica de la ESP32-C6 |
| `10..15` | `reserved` | `0x00 × 6` | Relleno reservado en cero |

### Flujo de aprovisionamiento y autoconfiguración

```text
  ESP32-C6 (No aprovisionada)                     ESP32-S3 Provisioner (0x0001)
             │                                                 │
             │── Unprovisioned Beacon (UUID: AA 55 01 <rol>) ─►│
             │                                                 │ Verifica magic 0xAA55
             │                                                 │ Mapea rol -> Unicast (0x0010/20/30)
             │◄─────────── PB-ADV Link Open + Invite ──────────│
             │◄═══════════ Intercambio ECDH + NetKey ═════════►│
             │                                                 │
             │◄── Config AppKey Add (NetIdx=0, AppIdx=0) ──────│
             │── Config AppKey Status (OK) ───────────────────►│
             │                                                 │
             │◄── Config Model App Bind (Vendor Server/Client)─│
             │── Config Model App Status (OK) ────────────────►│
             │                                                 │
             │◄── Config Model Pub/Sub Set (Grupos 0xC000..3) ─│
             │── Config Model Status (OK) ────────────────────►│
             │                                                 │
             │◄── Config Relay Set (ON, count=2, step=1) ──────│
             │── Config Relay Status (OK) ────────────────────►│
             │                                                 │
         [Guarda NVS]                                     [Guarda NVS]
```

### Persistencia y Factory Reset
- Todas las placas compilan con `CONFIG_BLE_MESH_SETTINGS=y` (NVS). Tras el primer aprovisionamiento, la red sobrevive a cortes eléctricos y reinicios inmediatos.
- Mantener presionado el botón **`BOOT` durante 5 segundos** (`GPIO 9` en C6, `GPIO 0` en S3) invoca la limpieza del almacenamiento BLE Mesh en NVS y reinicia la placa para permitir un nuevo aprovisionamiento limpio.

---

## 3. Especificación del Vendor Model (`CID = 0x02E5`)

Se define un **Vendor Model** propietario sobre `Company ID = 0x02E5` (Espressif Systems, uso académico):
- **Vendor Model Server ID:** `0x0001` (recibe comandos y estados)
- **Vendor Model Client ID:** `0x0002` (emite comandos y eventos)

En BLE Mesh, los opcodes de Vendor Model utilizan **3 octetos**: `0xC0..0xCF` (bits `[7:6] = 11`) seguido del Company ID en *little-endian* (`0xE5, 0x02`). Todos los campos multibyte viajan en **little-endian**.

### Catálogo de Opcodes

| Opcode (byte 0) | Nombre simbólico | Grupo / Destino | Sentido | ACK / Retrans. | Propósito |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`0xC0`** | `OP_ALARM_SET` | `0xC001` / `0x0020` | S3 → N2 / N1 → N2 | **Sí (crítico)** | Armar o desarmar alarma (desde Telegram o RFID válido) |
| **`0xC1`** | `OP_ALARM_EVENT` | `0xC001` / `0x0001` | N2 → S3 | **Sí (crítico)** | Reporte de transición (`DESARMADA`, `ARMADA`, `DISPARADA`) + causa |
| **`0xC2`** | `OP_RFID_AUTH_FAIL` | `0xC001` | N1 → N2, S3 | **Sí** | Notifica intento de tarjeta RFID no autorizada (incrementa contador en N2) |
| **`0xC3`** | `OP_LOCK_SET` | `0xC002` / `0x0010` | S3 → N1 | **Sí (crítico)** | Comando remoto `/cerradura abrir` o `/cerradura cerrar` |
| **`0xC4`** | `OP_LOCK_STATUS` | `0xC002` / `0x0001` | N1 → S3 | **Sí** | Evento de cambio en cerradura (abierta/cerrada, fuente RFID/remoto, UID) |
| **`0xC5`** | `OP_RFID_MGMT` | `0x0010` | S3 → N1 | **Sí** | Operaciones `/rfid list`, `/rfid add <UID>`, `/rfid del <UID>` |
| **`0xC6`** | `OP_RFID_MGMT_ACK` | `0x0001` | N1 → S3 | No | Respuesta con resultado y lista de UIDs almacenados en NVS de N1 |
| **`0xC7`** | `OP_CLIMA_TELEMETRY` | `0xC003` / `0x0001` | N3 → S3 | No (periódico / cambio) | Temperatura, humedad, modo y nivel PWM actual del ventilador |
| **`0xC8`** | `OP_FAN_SET` | `0xC003` / `0x0030` | S3 → N3 | **Sí** | Comando `/ventilador off|baja|media|alta|auto` |
| **`0xC9`** | `OP_THRESHOLD_SET` | `0xC003` / `0x0030` | S3 → N3 | **Sí** | Comando `/umbral <°C>` para ajustar `T_base` del modo AUTO |
| **`0xCA`** | `OP_HEARTBEAT` | `0xC000` / `0x0001` | C6 → S3 | No (cada 15 s) | Salud del nodo, `uptime`, `free_heap`, `deadline_misses`, `rssi_last` |
| **`0xCB`** | `OP_STATUS_REQ` | `0xC000` | S3 → C6 | No | Solicita reporte inmediato de estado (usado al arrancar S3 o `/estado`) |
| **`0xCC`** | `OP_METRICS_REPORT` | `0x0001` | C6 → S3 | No | Respuesta con estadísticas STR (`max_lat_us`, `avg_jitter_us`, `min_stack_w`, `%CPU`) |
| **`0xCD`** | `OP_DEMO_HOP_CTRL` | `0xC000` | S3 → C6 | **Sí** | Activa/desactiva modo `/mesh demo_hop on|off` (ajusta potencia TX en nodo objetivo) |
| **`0xCE`** | `OP_VENDOR_ACK` | Unicast origen | Receptor → Emisor | No | Confirmación genérica para opcodes críticos (`ack_opcode`, `seq_num`, `status`) |

---

### Estructura binaria (Byte-by-Byte Payload Layout)

Todos los mensajes críticos incluyen un **`seq_num` (1 byte, `0..255`)** para deduplicación e idempotencia.

#### 1. Seguridad y Alarma (`0xC0` – `0xC2`)

- **`OP_ALARM_SET` (3 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `target_state` (`0 = DESARMADA`, `1 = ARMADA`)
  - `byte[2]`: `source` (`1 = TELEGRAM`, `2 = RFID_C6_DIRECT`)
- **`OP_ALARM_EVENT` (9 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `new_state` (`0 = DESARMADA`, `1 = ARMADA`, `2 = DISPARADA`)
  - `byte[2]`: `reason` (`0 = CMD_TG`, `1 = RFID_OPEN`, `2 = RFID_CLOSE`, `3 = PIR_MOTION`, `4 = RFID_3_FAILS`)
  - `byte[3..6]`: `local_latency_us` (`uint32_t`, latencia medida desde ISR/comando hasta actuación LED en Nodo 2)
  - `byte[7]`: `fail_counter` (`uint8_t`, 0..3)
  - `byte[8]`: `flags` (`bit0 = pir_active`)
- **`OP_RFID_AUTH_FAIL` (10 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `uid_len` (`uint8_t`, típico 4 o 7)
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
  - `byte[4..10]`: `uid[7]` (`uint8_t[7]`, en ceros si el origen fue Telegram)
- **`OP_RFID_MGMT` (10 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `action` (`0 = LIST`, `1 = ADD`, `2 = DEL`)
  - `byte[2]`: `uid_len` (`uint8_t`)
  - `byte[3..9]`: `uid[7]` (`uint8_t[7]`)
- **`OP_RFID_MGMT_ACK` (variable, máx. 68 bytes — segmentado automáticamente por BLE Mesh si > 11 B):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `action` (`uint8_t`)
  - `byte[2]`: `status` (`0 = OK`, `1 = FULL`, `2 = NOT_FOUND`, `3 = ALREADY_EXISTS`)
  - `byte[3]`: `count` (`uint8_t`, cantidad total de tarjetas en NVS, máx. 8)
  - `byte[4..]`: lista compacta de tarjetas (`uid_len (1 B) + uid[7] (7 B)` × `count`)

#### 3. Climatización (`0xC7` – `0xC9`)

- **`OP_CLIMA_TELEMETRY` (8 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1..2]`: `temp_deci_c` (`int16_t`, ej. `265` = `26.5 °C`)
  - `byte[3]`: `humidity_pct` (`uint8_t`, `0..100 %`)
  - `byte[4]`: `fan_mode` (`0 = MANUAL`, `1 = AUTO`)
  - `byte[5]`: `fan_level` (`0 = OFF`, `1 = BAJA`, `2 = MEDIA`, `3 = ALTA`)
  - `byte[6]`: `threshold_base_c` (`uint8_t`, °C entero, defecto `25`)
  - `byte[7]`: `change_reason` (`0 = PERIODIC/REQ`, `1 = LEVEL_CHANGED`, `2 = SENSOR_ERR`)
- **`OP_FAN_SET` (2 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `cmd` (`0 = OFF`, `1 = BAJA`, `2 = MEDIA`, `3 = ALTA`, `4 = AUTO`)
- **`OP_THRESHOLD_SET` (2 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `new_t_base_c` (`uint8_t`, rango válido `15..40 °C`)

#### 4. Sistema, Métricas y Demo Multi-salto (`0xCA` – `0xCE`)

- **`OP_HEARTBEAT` (10 bytes):**
  - `byte[0]`: `node_role` (`1`, `2`, `3`)
  - `byte[1..4]`: `uptime_s` (`uint32_t`)
  - `byte[5..6]`: `free_heap_kb` (`uint16_t`)
  - `byte[7..8]`: `deadline_misses` (`uint16_t`)
  - `byte[9]`: `flags` (`bit0 = demo_hop_active`, `bit1 = sensor_ok`)
- **`OP_STATUS_REQ` (1 byte):**
  - `byte[0]`: `req_flags` (`0x01 = state`, `0x02 = str_metrics`, `0x03 = both`)
- **`OP_METRICS_REPORT` (15 bytes):**
  - `byte[0]`: `node_role` (`1`, `2`, `3`)
  - `byte[1..4]`: `max_crit_latency_us` (`uint32_t`)
  - `byte[5..8]`: `avg_jitter_us` (`uint32_t`)
  - `byte[9..10]`: `deadline_misses` (`uint16_t`)
  - `byte[11..12]`: `min_stack_free_bytes` (`uint16_t`)
  - `byte[13]`: `cpu_busy_pct` (`uint8_t`)
  - `byte[14]`: `last_rx_hops` (`uint8_t`)
- **`OP_DEMO_HOP_CTRL` (3 bytes):**
  - `byte[0]`: `seq_num` (`uint8_t`)
  - `byte[1]`: `enable` (`0 = NORMAL`, `1 = DEMO_MULTI_HOP`)
  - `byte[2]`: `target_unicast_low` (`0x30` = Nodo 3 por defecto)
- **`OP_VENDOR_ACK` (3 bytes):**
  - `byte[0]`: `ack_opcode` (`uint8_t`)
  - `byte[1]`: `seq_num` (`uint8_t`)
  - `byte[2]`: `status` (`0 = OK`, `1 = ERR_INVALID`)

---

## 4. Confiabilidad, Idempotencia y Conteo de Saltos (`hops`)

1. **Deduplicación e Idempotencia:** cada receptor mantiene el último `seq_num` procesado por dirección origen (`src_addr`) y opcode crítico. Si una retransmisión llega duplicada, **no re-ejecuta** el toggle ni el cambio de estado, pero **sí reenvía** `OP_VENDOR_ACK`.
2. **Retransmisión de mensajes críticos:** los mensajes marcados como críticos (`OP_ALARM_SET`, `OP_ALARM_EVENT`, `OP_LOCK_SET`, `OP_FAN_SET`, `OP_THRESHOLD_SET`, `OP_RFID_MGMT`, `OP_DEMO_HOP_CTRL`) esperan `OP_VENDOR_ACK` hasta **400 ms** y realizan hasta **3 reintentos** en una cola de salida dedicada, mitigando pérdidas momentáneas por coexistencia BLE/Wi-Fi en el ESP32-S3.
3. **Cálculo de saltos (TTL):** todas las placas transmiten con `tx_ttl = 5` (configurable). Al recibir cualquier mensaje en el callback del Vendor Model (`esp_ble_mesh_model_cb_param_t` → `ctx->recv_ttl`):
   $$\text{hops} = \text{TX\_TTL\_DEFAULT} - \text{ctx->recv\_ttl}$$
   - `hops == 0` → recepción directa (1 enlace de radio).
   - `hops >= 1` → trama reenviada a través de al menos un nodo **Relay** ESP32-C6.

---

## 5. Modo Demostración Multi-Salto (`/mesh demo_hop on|off`)

En una mesa de laboratorio o salón de clase, las 4 placas están a menos de 2 metros y se escuchan directamente (`hops == 0`). Para demostrar el **enrutamiento multi-salto (*Relay*)** en vivo sin tener que salir del salón:

1. Al ejecutar `/mesh demo_hop on` en Telegram (o activarlo por `menuconfig`):
   - El Gateway S3 envía `OP_DEMO_HOP_CTRL` (`enable = 1`, `target = Nodo 3 0x0030`) al grupo `0xC000`.
   - El **Nodo 3** reduce su potencia de transmisión BLE al mínimo (`ESP_PWR_LVL_N15` / `-15 dBm`).
   - En paralelo, para garantizar determinismo en una mesa corta, el **Gateway S3** activa un **filtro de demostración en capa de aplicación** que descarta paquetes provenientes de `0x0030` cuando `hops == 0` (directos) y acepta únicamente las copias que llegan con **`hops >= 1`** retransmitidas por el **Nodo 1** o el **Nodo 2**.
2. Como resultado, tanto en los logs seriales como en el comando `/estado` y en las métricas STR de la Fase 6, se evidencia el salto real (`hops: 1` o `hops: 2`) y el aumento medible de latencia entre 1 salto y 2 saltos sobre la malla física.
3. Con `/mesh demo_hop off` el sistema retorna inmediatamente a potencia TX nominal y recepción directa sin filtro.
