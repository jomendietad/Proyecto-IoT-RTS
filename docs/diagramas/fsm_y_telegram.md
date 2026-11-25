# Máquinas de Estados, Reglas Funcionales y Bot de Telegram

Este documento especifica el comportamiento funcional completo de los subsistemas de **Alarma tipo carro**, **Control de Acceso RFID**, **Alertas por Sobresalto Térmico** y la **interfaz de usuario en el grupo de Telegram**.

---

## 1. FSM de la Alarma "tipo carro" (Nodo 2 — `ESP32-C6`)

La alarma opera con **3 estados exactos** y **armado instantáneo** (sin estado intermedio de tiempo de gracia).

```text
               /armar (Telegram) o Cerradura cerrada por RFID (vía Maestro S3)
          ┌─────────────────────────────────────────────────────────────────────┐
          │                                                                     ▼
    [ DESARMADA ]                                                         [ ARMADA ]
     • LED: OFF                                                            • Confirmación: doble
     • Al entrar: 1 destello                                                 destello rápido (100 ms)
          ▲                                                                • LED reposo: OFF
          │                                                                     │
          │         /desarmar (Telegram)                                        │ PIR detecta movimiento
          │         o Cerradura abierta por RFID (vía S3)                       │         O
          │                                                                     │ 3 intentos consecutivos
          │                                                                     │ de RFID no autorizado
          │                                                                     ▼
          └───────────────────────────── [ DISPARADA ] ◄────────────────────────┘
                                          • LED: parpadeo rápido continuo INDEFINIDO (150 ms ON / 150 ms OFF)
                                          • Alerta prioritaria inmediata a Telegram
                                          • No se auto-silencia por tiempo
```

### Tabla de transiciones de la FSM de Alarma

| Estado actual | Evento / Disparador | Siguiente estado | Acción física (LED Nodo 2) | Acción en red / Telegram |
| :--- | :--- | :--- | :--- | :--- |
| **`DESARMADA`** | `/armar` en Telegram o **cierre de cerradura con RFID** (`N1 → S3 → N2`) | **`ARMADA`** | **Doble destello** rápido (100 ms ON, 100 ms OFF, 100 ms ON) como confirmación estilo carro. | Envía `OP_ALARM_EVENT` (`ARMADA`). Telegram notifica `"🛡️ Alarma ARMADA"`. |
| **`DESARMADA`** | Movimiento PIR (`GPIO 3` flanco ↑) | **`DESARMADA`** | Ninguna (el PIR no dispara alarma estando desarmada). | Se registra evento de presencia local en logs; sin spam en Telegram. |
| **`ARMADA`** | **Movimiento PIR** (`GPIO 3` flanco ↑) | **`DISPARADA`** | Inicia inmediatamente patrón **parpadeo rápido continuo indefinido** (150 ms ON / 150 ms OFF). | Envía `OP_ALARM_EVENT` (`DISPARADA`, `PIR_MOTION`). Telegram envía **alerta urgente**. |
| **`ARMADA`** | **RFID no autorizado** (`OP_RFID_AUTH_FAIL` < 3) | **`ARMADA`** | Incrementa `fail_counter` (`1/3`, `2/3`). | Telegram notifica intento fallido (`"⚠️ Tarjeta RFID rechazada (1/3)"`). |
| **`ARMADA`** | **3.er intento consecutivo de RFID no autorizado** | **`DISPARADA`** | Inicia **parpadeo rápido continuo indefinido**. | Envía `OP_ALARM_EVENT` (`DISPARADA`, `RFID_3_FAILS`). Telegram envía **alerta urgente**. |
| **`ARMADA`** | `/desarmar` en Telegram o **apertura con RFID autorizado** (`N1 → S3 → N2`) | **`DESARMADA`** | **1 destello breve** (150 ms) de confirmación y LED apagado; reinicia `fail_counter = 0`. | Envía `OP_ALARM_EVENT` (`DESARMADA`). Telegram notifica `"🔓 Alarma DESARMADA"`. |
| **`DISPARADA`** | `/desarmar` en Telegram o **tarjeta RFID autorizada** (`N1 → S3 → N2`) | **`DESARMADA`** | Detiene parpadeo inmediatamente, da **1 destello** de confirmación, apaga LED y reinicia `fail_counter = 0`. | Envía `OP_ALARM_EVENT` (`DESARMADA`). Telegram notifica `"✅ Alarma silenciada y DESARMADA"`. |

> **Regla de reinicio del contador de fallos:** `fail_counter` en Nodo 2 vuelve a `0` inmediatamente al pasar una tarjeta RFID autorizada o al desarmar la alarma.

---

## 2. Cerradura (Toggle) y Acoplamiento Estilo Carro (Nodo 1 → S3 → Nodo 2)

### Comportamiento de la cerradura (Nodo 1 — `ESP32-C6`)
- La cerradura emulada (`GPIO 20`) funciona siempre en modo **enclavado (*toggle*)**:
  - **`CERRADA` (`LED OFF`):** estado inicial seguro al arrancar.
  - **`ABIERTA` (`LED ON`):** permanece encendida indefinidamente hasta recibir otro acercamiento de tarjeta RFID autorizada o el comando `/cerradura cerrar` desde Telegram.

### Acoplamiento en la estrella BLE GATT (`N1 → Maestro S3 → N2`) vs. Control independiente
1. **Con tarjeta RFID autorizada en Nodo 1 (estilo carro completo):**
   - Si la cerradura estaba **`CERRADA`** → pasa a **`ABIERTA` (`LED ON`)** y Nodo 1 notifica `OP_ALARM_SET(DESARMADA, RFID_C6_DIRECT)` al Maestro S3, quien reenvía al instante hacia Nodo 2 para **desarmar o silenciar** la alarma.
   - Si la cerradura estaba **`ABIERTA`** → pasa a **`CERRADA` (`LED OFF`)** y Nodo 1 notifica `OP_ALARM_SET(ARMADA, RFID_C6_DIRECT)` al Maestro S3, quien reenvía al instante hacia Nodo 2 para **armar** la alarma.
2. **Desde Telegram (control granular independiente):**
   - `/cerradura abrir` y `/cerradura cerrar` actúan únicamente sobre el LED de cerradura del Nodo 1.
   - `/armar` y `/desarmar` actúan directamente sobre la alarma del Nodo 2.
3. **Autonomía sin Internet ni router Wi-Fi:** mientras el Maestro ESP32-S3 y las placas C6 estén energizadas, todo el enrutamiento en la estrella BLE GATT (`N1 ↔ S3 ↔ N2`) opera sin necesidad de Wi-Fi ni Internet.

### Gestión de tarjetas RFID en Nodo 1 (NVS local)
- El Nodo 1 arranca con el UID autorizado por defecto **`B6:7B:0C:03`** cuando su partición NVS está vacía.
- A partir de ahí, la lista válida reside en **NVS local de Nodo 1** (capacidad hasta 8 UIDs) y puede consultarse o modificarse en caliente desde el grupo de Telegram:
  - `/rfid list` → devuelve la lista numerada de UIDs registrados en Nodo 1.
  - `/rfid add <UID>` (ej. `/rfid add A1B2C3D4`) → agrega y persiste en NVS.
  - `/rfid del <UID>` → revoca y borra de NVS.

---

## 3. Alertas por Sobresalto Térmico (Nodo 3 — `ESP32-C6 SuperMini` + DHT11 RMT)

El Nodo 3 realiza muestreo periódico no bloqueante cada **3 s** mediante periférico **RMT** (`GPIO 2`) sin actuador de ventilador, evaluando **dos criterios simultáneos**:

| Condición evaluada | Regla exacta | Estado resultante (`iot_temp_alert_t`) | Notificación a Telegram |
| :--- | :--- | :--- | :--- |
| **Sobresalto térmico brusco** | **`|T_actual - T_previa| >= 2.0 °C`** (`|ΔT| >= 20` deci-°C) entre lecturas consecutivas | `TEMP_ALERT_SPIKE` (o `TEMP_ALERT_BOTH` si además `T >= T_high`) | **Inmediata (`PRIO_URGENT`):** `"🌡️⚠️ Sobresalto térmico detectado: T=... (ΔT=...)"` |
| **Superación de umbral superior** | **`T_actual >= T_high`** (defecto **`30 °C`**, ajustable con `/umbral <°C>` en `15..50 °C`, persistido en NVS) | `TEMP_ALERT_HIGH` | **Inmediata:** `"🔥 Temperatura alta: T=... >= T_high"` |
| **Normalización con histéresis** | Estando en `HIGH`, desciende por debajo de **`T_high - 1.0 °C`** | `TEMP_ALERT_NORMAL` | Notifica recuperación (`"✅ Temperatura normalizada"`), restaurando RGB |
| **Lectura estable sin cambios** | Sin sobresalto ni cruce de umbral | Se mantiene estado | **Sin envío (anti-spam):** visible al consultar `/estado` |

> En la primera lectura tras arrancar (`s_has_prev == false`), se inicializa la referencia previa con `ΔT = 0` para evitar falsos sobresaltos en el primer ciclo.

---

## 4. Interfaz del Bot de Telegram (Grupo autorizado)

### 4.1 Seguridad y permisos
- En `@BotFather` se desactiva **Group Privacy** (`/setprivacy` → `Disable`) para que el bot procese comandos e interacciones en el grupo.
- El Maestro ESP32-S3 valida obligatoriamente `message.chat.id == TELEGRAM_AUTHORIZED_CHAT_ID` (y `callback_query.message.chat.id == TELEGRAM_AUTHORIZED_CHAT_ID`) definido en `secrets.h`.
- **Permisos:** cualquier miembro perteneciente al grupo autorizado puede ejecutar comandos y pulsar botones del panel.

### 4.2 Catálogo de comandos de texto

| Comando | Parámetros | Acción ejecutada |
| :--- | :--- | :--- |
| `/panel` | *(ninguno)* | Envía el mensaje de resumen con el **Inline Keyboard** interactivo |
| `/estado` | *(ninguno)* | Muestra estado consolidado: alarma, cerradura, temp/hum/`ΔT`/alerta, umbral `T_high`, estado BLE de los 3 C6 y red Wi-Fi activa |
| `/armar` | *(ninguno)* | Arma instantáneamente la alarma en Nodo 2 |
| `/desarmar` | *(ninguno)* | Desarma / silencia la alarma en Nodo 2 |
| `/cerradura` | `abrir` \| `cerrar` | Activa o desactiva la cerradura en Nodo 1 |
| `/umbral` | `<°C>` (ej. `/umbral 28`) | Ajusta `T_high` (rango `15..50 °C`) en Nodo 3 y lo persiste en NVS |
| `/rfid` | `list` \| `add <UID>` \| `del <UID>` | Lista, registra o revoca tarjetas RFID en NVS del Nodo 1 |
| `/red` | *(ninguno)* | Consulta estado de las 3 conexiones BLE GATT en estrella (conectado/desconectado, RSSI y uptime) |
| `/metricas` | *(ninguno)* | Reporta estadísticas en vivo de STR (latencias µs, jitter, *deadline misses*, stack libre, SRAM/PSRAM libre y `% CPU`) |

### 4.3 Diseño del Inline Keyboard (`/panel`)

```text
┌───────────────────────────┬───────────────────────────┐
│     🛡️ Armar Alarma       │    🔓 Desarmar Alarma     │   (cmd:arm / cmd:disarm)
├───────────────────────────┼───────────────────────────┤
│   🔑 Abrir Cerradura      │    🔒 Cerrar Cerradura    │   (cmd:lock_open / cmd:lock_close)
├───────────────────────────┼───────────────────────────┤
│     📊 Estado Global      │     📡 Estado Red BLE     │   (cmd:status / cmd:net)
├───────────────────────────┴───────────────────────────┤
│              ⏱️ Métricas Tiempo Real (STR)            │   (cmd:metrics)
└───────────────────────────────────────────────────────┘
```

- Al presionar cualquier botón, el S3 procesa el `callback_query`, despacha la acción por BLE GATT (`0xFF12`), responde `answerCallbackQuery` para quitar el reloj de carga en Telegram y confirma el resultado en el chat.

---

## 5. Red Wi-Fi, Prioridad HTTPS y Manejo de Secretos

1. **Gestión de credenciales (`secrets.h` + `secrets.example.h`):**
   - `firmware/gateway_s3_uno/main/secrets.h` está excluido por [.gitignore](../../.gitignore) y usa una plantilla versionada `secrets.example.h` con `WIFI_PRIMARY_*`, `WIFI_BACKUP_*`, `TELEGRAM_BOT_TOKEN` y `TELEGRAM_AUTHORIZED_CHAT_ID`.
   - **Failover Wi-Fi automático:** si la red primaria falla al conectar o tras 3 reintentos (~15–20 s), el supervisor Wi-Fi del ESP32-S3 conmuta automáticamente a la red secundaria sin interrumpir los enlaces BLE GATT.
2. **Conexión HTTPS persistente y colas priorizadas:**
   - `task_tg_rx` mantiene *Long Polling* (`getUpdates?timeout=25`) con sesión TLS persistente (`keep-alive`).
   - `task_tg_tx` prioriza **`PRIO_URGENT`** (disparos de alarma, 3 fallos RFID, sobresaltos térmicos) por encima de **`PRIO_NORMAL`** (respuestas `/estado`, confirmaciones rutinarias).
