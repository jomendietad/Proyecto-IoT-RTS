# Sistema Distribuido IoT y de Tiempo Real para Automatización y Seguridad de Habitación (BLE Mesh + Border Router Telegram)

Proyecto académico integrador diseñado para cumplir simultáneamente los requerimientos técnicos de las asignaturas **Desarrollo de Sistemas IoT** y **Sistemas en Tiempo Real (STR)**.

El sistema implementa una red mallada inalámbrica basada en **Bluetooth Low Energy Mesh (BLE Mesh)** sobre **3 nodos ESP32-C6** (`2× DevKitC-1` + `1× SuperMini`) y **1 nodo central ESP32-S3 N16R8 (formato UNO)** que actúa como **Provisioner BLE Mesh, Gateway y Border Router Wi-Fi** hacia Internet mediante un **Bot de Telegram** operando en un grupo.

---

## 1. Resumen del Proyecto y Objetivos Académicos (IoT + STR)

El objetivo es monitorear, asegurar y automatizar una habitación mediante un sistema distribuido con **4 microcontroladores** que operan bajo restricciones temporales definidas, mantienen **autonomía local ante fallos de conectividad** (*offline-first* en la malla) y exponen telemetría y telecontrol bidireccional por Internet.

| Dimensión | Aportes principales del proyecto |
| :--- | :--- |
| **Desarrollo de Sistemas IoT** | • Red mallada **BLE Mesh (ESP-BLE-MESH)** con enrutamiento multi-salto (*Relay* + modo demo `/mesh demo_hop`), aprovisionamiento automático PB-ADV y persistencia NVS.<br>• Arquitectura **Gateway / Border Router** desacoplada (BLE Mesh ↔ Wi-Fi / HTTPS TLS ↔ Telegram Bot API con *Inline Keyboard* `/panel`).<br>• Interacción directa **M2M (C6 ↔ C6)** mediante modelo *Publish/Subscribe* por grupos (RFID desarma/arma alarma directamente).<br>• Resiliencia y seguridad: failover dual Wi-Fi (`secrets.h`), verificación estricta de `chat_id`, gestión dinámica `/rfid` en NVS y monitoreo por *heartbeats*. |
| **Sistemas en Tiempo Real (STR)** | • Diseño formal sobre **ESP-IDF puro + FreeRTOS** (con sustentación técnica de FreeRTOS sobre Zephyr) sin `delay()` bloqueantes en rutas críticas.<br>• Planificación por prioridades (Rate Monotonic / criticidad), **deadlines explícitos** y monitoreo de *deadline misses*.<br>• Uso de primitivas RTOS: `ISR → xTaskNotifyFromISR`, `xQueue`, `EventGroups`, `Mutex` (herencia de prioridad), `xTimer` y **Task Watchdog Timer (TWDT)**.<br>• **Core Pinning** SMP en el ESP32-S3 (Core 0 radio/coexistencia; Core 1 FSM y Telegram HTTPS).<br>• Lectura determinista del DHT11 usando periférico hardware **RMT** (sin *bit-banging*).<br>• **Instrumentación 100 % por software** (`esp_timer_get_time()` en µs, `vTaskGetRunTimeStats`, `uxTaskGetStackHighWaterMark`, `/metricas` en Telegram y análisis en `tools/`), sin pines GPIO de traza. |

---

## 2. Arquitectura del Sistema (4 placas: 3× ESP32-C6 + 1× ESP32-S3 N16R8 UNO)

### Diagrama general

```text
               [ Red BLE Mesh (ESP-BLE-MESH) — 4 nodos ]

  ┌─────────────────────────┐   ┌─────────────────────────┐   ┌─────────────────────────┐
  │ Nodo 1 (C6-DevKitC-1)   │   │ Nodo 2 (C6-DevKitC-1)   │   │ Nodo 3 (C6 SuperMini)   │
  │ Acceso                  │◄─►│ Seguridad / Alarma      │◄─►│ Climatización           │
  │ - RFID-RC522 (SPI, 3.3V)│   │ - PIR HC-SR501 (5V/3.3V)│   │ - DHT11 (RMT, 3.3V)     │
  │ - Cerradura (LED toggle)│   │ - Alarma (LED patrones) │   │ - Ventilador (LED PWM)  │
  │   [Relay activo]        │   │   [Relay activo]        │   │   [Relay activo]        │
  └────────────┬────────────┘   └────────────┬────────────┘   └────────────┬────────────┘
               │                             │                             │
               └─────────────────────────────┼─────────────────────────────┘
                                             ▼ (BLE Mesh directo, 1 o más saltos)
                         ┌───────────────────────────────────────┐
                         │      Gateway / Border Router          │
                         │     ESP32-S3 N16R8 (formato UNO)      │
                         │ - Provisioner BLE Mesh [NO-Relay]     │
                         │ - Coordinador + FSM Global + Métricas │
                         │ - Wi-Fi (dual failover) + Bot Telegram│
                         └───────────────────┬───────────────────┘
                                             │ Wi-Fi (HTTPS / Long Polling)
                                             ▼
                         ┌───────────────────────────────────────┐
                         │           Grupo de Telegram           │
                         │  Alertas, /panel (Inline) y Comandos  │
                         └───────────────────────────────────────┘
```

### Justificación técnica de eliminar el 4.º nodo RCP

En el ecosistema Espressif, el esquema **ESP32-C6 (RCP) + ESP32-S3 (Border Router)** es obligatorio cuando la malla usa **Thread o Zigbee (IEEE 802.15.4)**, porque el ESP32-S3 no tiene radio 802.15.4. Al utilizar **BLE Mesh**, el **ESP32-S3** posee radio **BLE 5.0 + Wi-Fi** integrados, por lo que prescindir del RCP:
1. Reduce latencia y elimina un protocolo puente Wi-Fi/UART adicional.
2. Dedica el 100 % de la radio de las **3 ESP32-C6** exclusivamente a **BLE Mesh** (máximo determinismo en campo).
3. Aprovecha los **2 núcleos (240 MHz)** y los **8 MB de Octal PSRAM** del **ESP32-S3 N16R8** para gestionar coexistencia BLE/Wi-Fi y TLS/HTTPS sin cuellos de botella de memoria.

### Distribución confirmada de placas y periféricos

| Placa | Modelo exacto | Periféricos asignados | Responsabilidad principal |
| :--- | :--- | :--- | :--- |
| **Nodo 1** | `ESP32-C6-DevKitC-1` | **RFID-RC522** (SPI, 3.3V) + **Cerradura** (LED toggle) + RGB integrado (`GPIO 8`) | Validación de UIDs desde NVS local, enclavamiento de cerradura, gestión remota `/rfid` y armado/desarme directo C6 ↔ C6 de la alarma. |
| **Nodo 2** | `ESP32-C6-DevKitC-1` | **PIR HC-SR501** (5V alim., 3.3V señal) + **Alarma tipo carro** (LED, sin buzzer) + RGB (`GPIO 8`) | Detección de movimiento por interrupción (`ISR → TaskNotify`), patrones de alarma automotriz y disparo por PIR o 3 intentos RFID inválidos. |
| **Nodo 3** | `ESP32-C6 SuperMini` | **DHT11** (bus 1-wire vía **RMT**) + **Ventilador DC** (LED **PWM `LEDC`** 3 niveles) + RGB (`GPIO 8`) | Lectura periódica no bloqueante cada 2–5 s y regulación automática con histéresis de 1 °C o control manual desde Telegram. |
| **Gateway** | `ESP32-S3 N16R8` (formato UNO) | Sin sensores externos + botón `BOOT` (`GPIO 0`) + RGB integrado | Provisioner BLE Mesh automático (*NO-Relay*), tabla global de estado, failover dual Wi-Fi, cliente HTTPS Telegram (`/panel`) y recolector de métricas STR. |

> **Nota sobre ESP32-S3 N16R8 UNO:** habilitar `CONFIG_SPIRAM=y` y `CONFIG_SPIRAM_MODE_OCT=y` para alojar buffers mbedTLS/HTTP/JSON en PSRAM (manteniendo stacks de tareas críticas en SRAM interna), y evitar los pines reservados **`GPIO 33–37`** (Octal PSRAM), **`GPIO 26–32`** (Flash) y **`GPIO 19–20`** (USB nativo).

---

## 3. Justificación del Stack: ESP-IDF puro vs. Zephyr

Se confirma **ESP-IDF puro (v6.1.1) + FreeRTOS** frente a Zephyr RTOS por razones de riesgo técnico y rigor en Sistemas en Tiempo Real:

| Criterio | ESP-IDF puro (v6.1.1 + FreeRTOS) | Zephyr RTOS |
| :--- | :--- | :--- |
| **Coexistencia Wi-Fi + BLE Mesh** | Soporte oficial maduro (`esp_ble_mesh` + árbitro `esp_coex` validado en ESP32-S3). | Alta fricción para Wi-Fi + BLE simultáneos sobre blobs binarios de Espressif. |
| **Periféricos clave (RMT, Octal PSRAM, TWDT)** | Drivers nativos listos para RMT (DHT11 no bloqueante en C6) y PSRAM Octal (`R8`) en S3. | Configuraciones complejas de *Devicetree overlays* y soporte desigual de periféricos propios. |
| **Sustentación académica STR** | Expone directamente **Fixed-Priority Preemptive Scheduling**, herencia de prioridad en Mutex, `xTaskNotifyFromISR`, **Core Pinning** SMP y `vTaskGetRunTimeStats`. | Su principal fortaleza (portabilidad entre múltiples fabricantes como Nordic/STM32) no aporta valor en un sistema 100 % Espressif. |

---

## 4. Red BLE Mesh: Aprovisionamiento Automático e Interacción Directa C6 ↔ C6

- **Aprovisionamiento automático (PB-ADV en ESP32-S3):** sin app móvil; cada C6 anuncia un Device UUID con prefijo `0xAA 0x55`, versión `0x01` y byte de rol (`0x01..0x03`). El S3 asigna direcciones Unicast fijas (`0x0001` Gateway, `0x0010` Nodo 1, `0x0020` Nodo 2, `0x0030` Nodo 3), distribuye `AppKey`, configura grupos Pub/Sub y activa *Relay* únicamente en las C6. Todo se persiste en **NVS** (`CONFIG_BLE_MESH_SETTINGS=y`), con *factory reset* al mantener `BOOT` durante **5 s**.
- **Interacción directa C6 ↔ C6 (`Vendor Model`, `CID = 0x02E5`):**
  - **`0xC000` (`GRP_SYSTEM`):** *heartbeats* (cada 15 s, timeout offline 45 s), `/estado`, métricas STR y control de demo multi-salto.
  - **`0xC001` (`GRP_SECURITY`):** puente directo **Nodo 1 ↔ Nodo 2**. Tarjeta RFID válida al abrir cerradura desarma/silencia la alarma; al cerrar cerradura arma la alarma al instante. Tres fallos RFID seguidos disparan la alarma. Funciona **100 % offline** aunque el Wi-Fi o el S3 estén apagados.
  - **`0xC002` (`GRP_ACCESS`):** comandos `/cerradura abrir|cerrar` y gestión `/rfid list|add|del`.
  - **`0xC003` (`GRP_CLIMA`):** telemetría DHT11, `/ventilador` y `/umbral`.
- **Demostración multi-salto en escritorio (`/mesh demo_hop on|off`):** reporta saltos reales (`hops = tx_ttl - rx_ttl`) en logs y `/estado`, permitiendo bajar potencia TX BLE en el Nodo 3 y filtrar paquetes directos (`hops == 0`) en el S3 para probar enrutamiento vía *Relay* (`hops >= 1`) sobre la misma mesa.

---

## 5. Lógica de Alarma ("tipo carro"), Climatización y Bot de Telegram

- **FSM de Alarma (3 estados exactos, armado instantáneo sin tiempo de gracia):**
  - **`DESARMADA`**: confirmada con **1 destello** al entrar; LED apagado.
  - **`ARMADA`**: confirmada al instante con **doble destello rápido**; armada por `/armar` o al cerrar la cerradura con RFID.
  - **`DISPARADA`**: activada al instante por **PIR** o tras **3 intentos consecutivos de RFID no autorizado**; **parpadeo rápido continuo indefinido** + alerta urgente en Telegram hasta `/desarmar` o tarjeta RFID válida.
- **Cerradura (Nodo 1):** modo **enclavado (*toggle*)** en `GPIO 20`; tarjeta autorizada alterna `ABIERTA`/`CERRADA` y arrastra a la alarma (estilo carro), mientras `/cerradura abrir|cerrar` en Telegram controla únicamente la cerradura.
- **Climatización (Nodo 3 — PWM `LEDC`):** modo `AUTO` con **histéresis de 1.0 °C** respecto a `T_base` (defecto `25 °C`, ajustable con `/umbral <°C>`):
  - `< 25 °C`: `OFF` (`0 %`) | `25–27 °C`: `BAJA` (`~35 %`) | `28–30 °C`: `MEDIA` (`~70 %`) | `>= 31 °C`: `ALTA` (`100 %`).
  - Notifica al grupo solo ante cambios de nivel/umbral o bajo consulta `/estado` (sin spam periódico).
- **Bot de Telegram en grupo autorizado:**
  - Comandos de texto (`/panel`, `/estado`, `/armar`, `/desarmar`, `/cerradura`, `/ventilador`, `/umbral`, `/rfid`, `/mesh`, `/metricas`) + **botones interactivos (*Inline Keyboard* en `/panel`)**.
  - Acceso verificado por `chat_id` del grupo (cualquier miembro del grupo opera); *Group Privacy* desactivado en `@BotFather`.
  - Credenciales en `secrets.h` (ignorado por Git, con plantilla `secrets.example.h` versionada) soportando **2 redes Wi-Fi con failover automático** (casa + hotspot móvil).

---

## 6. Documentación Técnica y Replicación (`docs/` y `REQUIREMENTS.md`)

Las especificaciones detalladas de arquitectura, pinout, protocolo binario, lógica funcional, entorno de desarrollo y requerimientos se encuentran en:

- [**Requerimientos y Guía de Replicación (`REQUIREMENTS.md`)**](REQUIREMENTS.md) — Requisitos agnósticos de software (Linux/Windows/macOS), BOM completo con alternativas de hardware y niveles de replicación sin depender de placas físicas.
- [**Entorno de Desarrollo en Linux (`ESP-IDF v6.1.1`)**](docs/entorno_desarrollo.md) — Instalación reproducible en Fedora 45 con Python 3.13 aislado, verificación agnóstica de permisos seriales/`udev` y flujo diario por terminal.
- [**Pinout y Especificaciones de Hardware**](docs/hardware/pinout.md) — Mapeo GPIO de las 4 placas (`2× C6-DevKitC-1`, `C6 SuperMini`, `S3 N16R8 UNO`), restricciones de pines, alimentación y código RGB de estado.
- [**Protocolo BLE Mesh y Vendor Model**](docs/diagramas/protocolo_ble_mesh.md) — Device UUID, aprovisionamiento automático PB-ADV, direcciones Unicast/Grupos, opcodes y payloads byte a byte, ACKs y modo `/mesh demo_hop`.
- [**Máquinas de Estados (FSM) y Bot de Telegram**](docs/diagramas/fsm_y_telegram.md) — Transiciones de alarma tipo carro, toggle de cerradura, gestión NVS `/rfid`, escalones PWM/histéresis, diseño del `/panel` *Inline Keyboard* y failover Wi-Fi.
- [**Diseño Formal RTOS y Plan Experimental STR**](docs/metricas-str/diseno_rtos_str.md) — Sustentación FreeRTOS vs. Zephyr, tablas formales de tareas/prioridades/deadlines, *Core Pinning*, TWDT e instrumentación por software (`esp_timer_get_time()` + `/metricas`).

---

## 7. Estructura del Repositorio

```text
Proyecto-IoT-RTS/
├── README.md                                 # Visión integral, decisiones y roadmap
├── REQUIREMENTS.md                           # Requerimientos multiplataforma, BOM y equivalencias
├── .gitignore                                # Exclusiones ESP-IDF, build/, sdkconfig y secrets.h
├── docs/
│   ├── entorno_desarrollo.md                 # Entorno ESP-IDF v6.1.1 en Linux, permisos y flujo CLI
│   ├── diagramas/
│   │   ├── protocolo_ble_mesh.md             # Contrato Vendor Model, PB-ADV y demo_hop
│   │   └── fsm_y_telegram.md                 # FSM alarma/cerradura/clima y UI Telegram
│   ├── hardware/
│   │   └── pinout.md                         # Pinout de las 4 placas y código LED RGB
│   └── metricas-str/
│       └── diseno_rtos_str.md                # Tablas RTOS, deadlines, FreeRTOS vs Zephyr y E1–E6
├── components/
│   ├── common_types/                         # Estructuras compartidas, estados y códigos de error
│   └── mesh_protocol/                        # Vendor Model, opcodes, serialización y ACKs
├── firmware/
│   ├── nodo1_acceso_c6/                      # Proyecto ESP-IDF (esp32c6 DevKitC-1): RC522 + Cerradura
│   ├── nodo2_seguridad_c6/                   # Proyecto ESP-IDF (esp32c6 DevKitC-1): PIR + Alarma
│   ├── nodo3_clima_c6/                       # Proyecto ESP-IDF (esp32c6 SuperMini): DHT11 + PWM
│   └── gateway_s3_uno/                       # Proyecto ESP-IDF (esp32s3 N16R8 UNO): Provisioner + Telegram
└── tools/                                    # Scripts Python para procesar logs [STR_METRIC]
```

*(Los directorios bajo `components/`, `firmware/` y `tools/` mantienen archivos `.gitkeep` temporales hasta incorporar el código fuente de cada fase).*

---

## 8. Plan de Acción por Fases

- [x] **Fase 1 — Pinout, contratos BLE Mesh y diseño RTOS**
  - Definición cerrada de hardware (`2× C6-DevKitC-1`, `C6 SuperMini`, `S3 N16R8 UNO`), protocolo BLE Mesh (`Vendor Model` + PB-ADV), FSMs, interfaz Telegram `/panel` y tablas formales STR documentadas en `docs/`.
- [ ] **Fase 2 — Drivers y pruebas unitarias FreeRTOS en las 3 ESP32-C6**
  - Crear proyectos base en `components/` y `firmware/`, validar SPI RC522 + NVS + cerradura (Nodo 1), interrupción PIR + patrones LED alarma (Nodo 2), DHT11 por RMT + PWM `LEDC` (Nodo 3) y márgenes de stack/TWDT sin red.
- [ ] **Fase 3 — Red BLE Mesh completa (aprovisionamiento automático, C6 ↔ C6 y multi-salto)**
  - Implementar Provisioner PB-ADV en ESP32-S3, persistencia NVS + *factory reset* (`BOOT` 5 s), comunicación directa `0xC001` (RFID ↔ Alarma) y prueba física/escritorio de *Relay* (`/mesh demo_hop`).
- [ ] **Fase 4 — Coexistencia Wi-Fi + BLE Mesh y Bot de Telegram en el ESP32-S3**
  - Configurar Octal PSRAM (`N16R8`), `esp_coex`, *Core Pinning* (Core 0/Core 1), failover dual Wi-Fi (`secrets.h`), *Long Polling* HTTPS (`keep-alive`), comandos de texto y `/panel` *Inline Keyboard*.
- [ ] **Fase 5 — Lógica integral, gestión `/rfid` y pruebas en modo offline/degradado**
  - Validar flujo completo de alarma (incluidos 3 fallos RFID), alta/baja remota de tarjetas en NVS y autonomía de la malla sin Wi-Fi ni Internet.
- [ ] **Fase 6 — Mediciones de Tiempo Real (E1–E6) y documentación de resultados**
  - Recolectar logs `[STR_METRIC]`, procesar percentiles/WCRT/jitter en `tools/`, registrar resultados de coexistencia en `docs/metricas-str/` y consolidar entregas de **IoT** y **STR**.
