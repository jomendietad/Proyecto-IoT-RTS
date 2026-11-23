# Máquinas de Estados, Reglas Funcionales y Bot de Telegram (Fase 1)

Este documento especifica el comportamiento funcional completo de los subsistemas de **Alarma tipo carro**, **Control de Acceso RFID**, **Climatización automática PWM** y la **interfaz de usuario en el grupo de Telegram**.

---

## 1. FSM de la Alarma "tipo carro" (Nodo 2 — `ESP32-C6`)

La alarma opera con **3 estados exactos** y **armado instantáneo** (sin estado intermedio de tiempo de gracia).

```text
               /armar (Telegram) o Cerradura cerrada por RFID (Grupo 0xC001)
          ┌─────────────────────────────────────────────────────────────────────┐
          │                                                                     ▼
    [ DESARMADA ]                                                         [ ARMADA ]
     • LED: OFF                                                            • Confirmación: doble
     • Al entrar: 1 destello                                                 destello rápido (100 ms)
          ▲                                                                • LED reposo: pulso corto
          │                                                                  cada 2 s (o OFF)
          │                                                                     │
          │         /desarmar (Telegram)                                        │ PIR detecta movimiento
          │         o Cerradura abierta por RFID (0xC001)                       │         O
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
| **`DESARMADA`** | `/armar` en Telegram o **cierre de cerradura con RFID** (`0xC001`) | **`ARMADA`** | **Doble destello** rápido (100 ms ON, 100 ms OFF, 100 ms ON) como confirmación estilo carro. | Publica `OP_ALARM_EVENT` (`ARMADA`). Telegram notifica `"🛡️ Alarma ARMADA"`. |
| **`DESARMADA`** | Movimiento PIR (`GPIO 3` flanco ↑) | **`DESARMADA`** | Ninguna (el PIR no dispara alarma estando desarmada). | Se registra evento de presencia local en logs; sin spam en Telegram. |
| **`ARMADA`** | **Movimiento PIR** (`GPIO 3` flanco ↑) | **`DISPARADA`** | Inicia inmediatamente patrón **parpadeo rápido continuo indefinido** (150 ms ON / 150 ms OFF). | Publica `OP_ALARM_EVENT` (`DISPARADA`, `PIR_MOTION`). Telegram envía **alerta urgente**. |
| **`ARMADA`** | **RFID no autorizado** (`OP_RFID_AUTH_FAIL` < 3) | **`ARMADA`** | Incrementa `fail_counter` (`1/3`, `2/3`). | Telegram notifica intento fallido (`"⚠️ Tarjeta RFID rechazada (1/3)"`). |
| **`ARMADA`** | **3.er intento consecutivo de RFID no autorizado** | **`DISPARADA`** | Inicia **parpadeo rápido continuo indefinido**. | Publica `OP_ALARM_EVENT` (`DISPARADA`, `RFID_3_FAILS`). Telegram envía **alerta urgente**. |
| **`ARMADA`** | `/desarmar` en Telegram o **apertura con RFID autorizado** (`0xC001`) | **`DESARMADA`** | **1 destello breve** (150 ms) de confirmación y LED apagado; reinicia `fail_counter = 0`. | Publica `OP_ALARM_EVENT` (`DESARMADA`). Telegram notifica `"🔓 Alarma DESARMADA"`. |
| **`DISPARADA`** | `/desarmar` en Telegram o **tarjeta RFID autorizada** (`0xC001`) | **`DESARMADA`** | Detiene parpadeo inmediatamente, da **1 destello** de confirmación, apaga LED y reinicia `fail_counter = 0`. | Publica `OP_ALARM_EVENT` (`DESARMADA`). Telegram notifica `"✅ Alarma silenciada y DESARMADA"`. |

> **Regla de reinicio del contador de fallos:** `fail_counter` en Nodo 2 vuelve a `0` inmediatamente al pasar una tarjeta RFID autorizada o al desarmar la alarma.

---

## 2. Cerradura (Toggle) y Acoplamiento Estilo Carro (Nodo 1 ↔ Nodo 2)

### Comportamiento de la cerradura (Nodo 1 — `ESP32-C6`)
- La cerradura emulada (`GPIO 20`) funciona siempre en modo **enclavado (*toggle*)**:
  - **`CERRADA` (`LED OFF`):** estado inicial seguro al arrancar (o último estado persistido si se configura).
  - **`ABIERTA` (`LED ON`):** permanece encendida indefinidamente hasta recibir otro acercamiento de tarjeta RFID autorizada o el comando `/cerradura cerrar` desde Telegram.

### Acoplamiento directo C6 ↔ C6 (Grupo `0xC001`) vs. Control independiente
1. **Con tarjeta RFID autorizada en Nodo 1 (estilo carro completo):**
   - Si la cerradura estaba **`CERRADA`** → pasa a **`ABIERTA` (`LED ON`)** y Nodo 1 publica `OP_ALARM_SET(DESARMADA, RFID_C6_DIRECT)` al grupo `0xC001`, **desarmando o silenciando instantáneamente** la alarma en Nodo 2.
   - Si la cerradura estaba **`ABIERTA`** → pasa a **`CERRADA` (`LED OFF`)** y Nodo 1 publica `OP_ALARM_SET(ARMADA, RFID_C6_DIRECT)` al grupo `0xC001`, **armando instantáneamente** la alarma en Nodo 2.
2. **Desde Telegram (control granular independiente):**
   - `/cerradura abrir` y `/cerradura cerrar` actúan únicamente sobre el LED de cerradura del Nodo 1 (no arman ni desarman automáticamente la alarma para permitir probar o controlar cada componente por separado).
   - `/armar` y `/desarmar` actúan directamente sobre la alarma del Nodo 2.
3. **Autonomía sin Internet ni ESP32-S3:** al viajar por el grupo BLE Mesh `0xC001`, toda la interacción RFID ↔ Alarma continúa operando normalmente si el router Wi-Fi se apaga o el S3 está fuera de línea.

### Gestión de tarjetas RFID en Nodo 1 (NVS local)
- El Nodo 1 arranca con una **lista base de UIDs autorizados** compilada en firmware la primera vez que su partición NVS está vacía.
- A partir de ahí, la lista válida reside en **NVS local de Nodo 1** (capacidad hasta 8 UIDs) y puede consultarse o modificarse en caliente desde el grupo de Telegram sin recompilar:
  - `/rfid list` → devuelve la lista numerada de UIDs registrados en Nodo 1.
  - `/rfid add <UID>` (ej. `/rfid add A1B2C3D4`) → agrega y persiste en NVS.
  - `/rfid del <UID>` → revoca y borra de NVS.

---

## 3. Control de Climatización (Nodo 3 — DHT11 + PWM `LEDC`)

El Nodo 3 lee el DHT11 cada **2–5 s** vía periférico **RMT** y controla la intensidad del LED del ventilador mediante **PWM `LEDC` (5 kHz, 10 bits)**.

### Escalones de velocidad en modo `AUTO`

Respecto a la temperatura base configurable **`T_base`** (por defecto **`25.0 °C`**, modificable con `/umbral <°C>`):

| Nivel PWM | Duty Cycle | Rango al subir (`T` creciente) | Umbral de bajada (**histéresis 1.0 °C**) | Equivalente por defecto (`T_base = 25 °C`) |
| :--- | :--- | :--- | :--- | :--- |
| **`OFF`** | `0 %` | `< T_base` | `< T_base - 1.0 °C` | `< 25.0 °C` (baja de `BAJA` a `OFF` en `< 24.0 °C`) |
| **`BAJA`** | `~35 %` (`358/1023`) | `[T_base, T_base + 3.0)` | `< T_base + 2.0 °C` | `25.0 – 27.9 °C` (baja de `MEDIA` a `BAJA` en `< 27.0 °C`) |
| **`MEDIA`** | `~70 %` (`716/1023`) | `[T_base + 3.0, T_base + 6.0)` | `< T_base + 5.0 °C` | `28.0 – 30.9 °C` (baja de `ALTA` a `MEDIA` en `< 30.0 °C`) |
| **`ALTA`** | `100 %` (`1023/1023`) | `>= T_base + 6.0 °C` | `>= T_base + 5.0 °C` | `>= 31.0 °C` |

- **Modo `MANUAL`:** al ejecutar `/ventilador off|baja|media|alta`, el Nodo 3 fija el duty solicitado e ignora las variaciones del DHT11 hasta que el usuario envíe `/ventilador auto`.
- **Política anti-spam en Telegram:** el Nodo 3 solo provoca notificación proactiva al grupo cuando **cambia de nivel de velocidad** (o al cambiar modo/umbral) o ante fallo persistente de lectura del sensor; las lecturas estables solo se muestran al pedir `/estado`.

---

## 4. Interfaz del Bot de Telegram (Grupo autorizado)

### 4.1 Seguridad y permisos
- En `@BotFather` se desactiva **Group Privacy** (`/setprivacy` → `Disable`) para que el bot procese comandos e interacciones en el grupo.
- El Gateway ESP32-S3 valida obligatoriamente `message.chat.id == TELEGRAM_AUTHORIZED_CHAT_ID` (y `callback_query.message.chat.id == TELEGRAM_AUTHORIZED_CHAT_ID`) definido en `secrets.h`.
- **Permisos:** cualquier miembro perteneciente al grupo autorizado puede ejecutar comandos y pulsar botones del panel.

### 4.2 Catálogo de comandos de texto

| Comando | Parámetros | Acción ejecutada |
| :--- | :--- | :--- |
| `/panel` | *(ninguno)* | Envía el mensaje de resumen con el **Inline Keyboard** interactivo |
| `/estado` | *(ninguno)* | Solicita/muestra estado consolidado: alarma, cerradura, temp/hum, ventilador, umbral, estado de los 3 C6, última red Wi-Fi y **saltos (`hops`)** de cada nodo |
| `/armar` | *(ninguno)* | Arma instantáneamente la alarma en Nodo 2 |
| `/desarmar` | *(ninguno)* | Desarma / silencia la alarma en Nodo 2 |
| `/cerradura` | `abrir` \| `cerrar` | Activa o desactiva la cerradura en Nodo 1 |
| `/ventilador` | `off` \| `baja` \| `media` \| `alta` \| `auto` | Cambia velocidad manual o devuelve a modo automático en Nodo 3 |
| `/umbral` | `<°C>` (ej. `/umbral 26`) | Ajusta `T_base` (rango `15..40 °C`) en Nodo 3 |
| `/rfid` | `list` \| `add <UID>` \| `del <UID>` | Lista, registra o revoca tarjetas RFID en NVS del Nodo 1 |
| `/mesh` | `status` \| `demo_hop on` \| `demo_hop off` | Consulta topología/TTL de nodos o activa el modo demostración multi-salto |
| `/metricas` | *(ninguno)* | Reporta estadísticas en vivo de STR (latencias µs, jitter, *deadline misses*, stack libre, SRAM/PSRAM libre y `% CPU`) |

### 4.3 Diseño del Inline Keyboard (`/panel`)

Al enviar `/panel`, el bot responde con un teclado interactivo en línea organizado por subsistema:

```text
┌───────────────────────────┬───────────────────────────┐
│     🛡️ Armar Alarma       │    🔓 Desarmar Alarma     │   (cmd:arm / cmd:disarm)
├───────────────────────────┼───────────────────────────┤
│   🔑 Abrir Cerradura      │    🔒 Cerrar Cerradura    │   (cmd:lock_open / cmd:lock_close)
├─────────────┬─────────────┼─────────────┬─────────────┤
│ 🌀 Auto     │ 🌀 Baja     │ 🌀 Media    │ 🌀 Alta     │   (cmd:fan_auto/low/med/high)
├─────────────┴─────────────┼─────────────┴─────────────┤
│     🛑 Ventilador OFF     │     📊 Estado Global      │   (cmd:fan_off / cmd:status)
├───────────────────────────┴───────────────────────────┤
│              ⏱️ Métricas Tiempo Real (STR)            │   (cmd:metrics)
└───────────────────────────────────────────────────────┘
```

- Al presionar cualquier botón, el S3 procesa el `callback_query`, despacha la acción por BLE Mesh (esperando ACK en comandos críticos), responde inmediatamente `answerCallbackQuery` para quitar el reloj de carga en Telegram y actualiza/confirma el estado en el chat.

---

## 5. Red Wi-Fi, Prioridad HTTPS y Manejo de Secretos

1. **Gestión de credenciales (`secrets.h` + `secrets.example.h`):**
   - El archivo local `firmware/gateway_s3_uno/main/secrets.h` está excluido por [.gitignore](../../.gitignore) y se acompaña de una plantilla versionada `secrets.example.h` con:
     - `WIFI_PRIMARY_SSID` / `WIFI_PRIMARY_PASS` (red habitual, p. ej. casa)
     - `WIFI_BACKUP_SSID` / `WIFI_BACKUP_PASS` (red de respaldo, p. ej. hotspot del celular en la universidad)
     - `TELEGRAM_BOT_TOKEN`
     - `TELEGRAM_AUTHORIZED_CHAT_ID`
   - **Failover Wi-Fi automático:** si la red primaria falla al conectar o tras 3 reintentos de reconexión (~15–20 s), el gestor Wi-Fi del ESP32-S3 conmuta automáticamente a la red secundaria sin bloquear la malla BLE Mesh.
2. **Conexión HTTPS persistente y colas priorizadas:**
   - `task_tg_rx` mantiene *Long Polling* (`getUpdates?timeout=25`) reutilizando sesión TLS (`keep-alive`) y certificado raíz en `esp_crt_bundle`.
   - `task_tg_tx` consume dos colas (o una cola con prioridad): **`PRIO_URGENT`** (disparos de alarma, 3 fallos RFID) siempre se transmite antes que **`PRIO_NORMAL`** (cambios de ventilador, respuestas `/estado` o telemetría).
