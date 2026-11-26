# Sistema Distribuido IoT y de Tiempo Real para Automatización y Seguridad de Habitación (BLE GATT Estrella + Border Router Telegram)

Proyecto académico integrador diseñado para cumplir simultáneamente los requerimientos técnicos de las asignaturas **Desarrollo de Sistemas IoT** y **Sistemas en Tiempo Real (STR)**.

El sistema implementa una red inalámbrica en **topología estrella sobre Bluetooth Low Energy (BLE GATT / NimBLE)** con **3 nodos periféricos ESP32-C6** (`2× DevKitC-1` + `1× SuperMini`) conectados directamente a **1 nodo central maestro ESP32-S3 N16R8 (formato UNO)** que actúa como **Maestro BLE GATT, Coordinador, Gateway y Border Router Wi-Fi** hacia Internet mediante un **Bot de Telegram** operando en un grupo.

---

## 1. Resumen del Proyecto y Objetivos Académicos (IoT + STR)

El objetivo es monitorear, asegurar y automatizar una habitación mediante un sistema distribuido con **4 microcontroladores** que operan bajo restricciones temporales definidas, mantienen **autonomía local ante fallos de Internet/Wi-Fi** (mientras el Maestro S3 opera en estrella local) y exponen telemetría y telecontrol bidireccional por Internet.

| Dimensión | Aportes principales del proyecto |
| :--- | :--- |
| **Desarrollo de Sistemas IoT** | • Red **BLE GATT en estrella (NimBLE)**: el ESP32-S3 actúa como **Maestro/Central** con 3 conexiones directas simultáneas (`Notify` + `Write with Response`) hacia las 3 ESP32-C6 periféricas, con reconexión automática.<br>• Arquitectura **Gateway / Border Router** desacoplada (BLE GATT ↔ Wi-Fi / HTTPS TLS ↔ Telegram Bot API con *Inline Keyboard* `/panel`).<br>• Coordinación estilo carro **N1 → S3 Maestro → N2** (RFID desarma/arma alarma sin depender de Internet).<br>• Resiliencia y seguridad: failover dual Wi-Fi (`secrets.h`), verificación estricta de `chat_id`, gestión dinámica `/rfid` en NVS y monitoreo por *heartbeats*. |
| **Sistemas en Tiempo Real (STR)** | • Diseño formal sobre **ESP-IDF v6.1.1 puro + FreeRTOS** (con sustentación técnica de FreeRTOS sobre Zephyr) sin `delay()` bloqueantes en rutas críticas.<br>• Planificación por prioridades (Rate Monotonic / criticidad), **deadlines explícitos** y monitoreo de *deadline misses*.<br>• Uso de primitivas RTOS: `ISR → xTaskNotifyFromISR`, `xQueue`, `EventGroups`, `Mutex` (herencia de prioridad), `xTimer` y **Task Watchdog Timer (TWDT)**.<br>• **Core Pinning** SMP en el ESP32-S3 (Core 0 radio/coexistencia; Core 1 FSM y Telegram HTTPS).<br>• Lectura determinista del DHT11 usando periférico hardware **RMT** (sin *bit-banging*) para detección inmediata de sobresaltos térmicos.<br>• **Instrumentación 100 % por software** (`esp_timer_get_time()` en µs, `vTaskGetRunTimeStats`, `uxTaskGetStackHighWaterMark`, `/metricas` en Telegram y análisis en `tools/`), sin pines GPIO de traza. |

---

## 2. Arquitectura del Sistema (4 placas: 3× ESP32-C6 + 1× ESP32-S3 N16R8 UNO)

### Diagrama general (Topología Estrella BLE GATT)

```text
           [ Red BLE GATT en Estrella (NimBLE) — 1 Maestro + 3 Periféricos ]

  ┌─────────────────────────┐   ┌─────────────────────────┐   ┌─────────────────────────┐
  │ Nodo 1 (C6-DevKitC-1)   │   │ Nodo 2 (C6-DevKitC-1)   │   │ Nodo 3 (C6 SuperMini)   │
  │ Acceso [Periférico]     │   │ Seguridad [Periférico]  │   │ Clima [Periférico]      │
  │ - RFID-RC522 (SPI, 3.3V)│   │ - PIR HC-SR501 (5V/3.3V)│   │ - DHT11 (RMT, 3.3V)     │
  │ - Cerradura (LED toggle)│   │ - Alarma (LED patrones) │   │ - Alertas sobresalto T  │
  └────────────┬────────────┘   └────────────┬────────────┘   └────────────┬────────────┘
               │                             │                             │
               └─────────────────────────────┼─────────────────────────────┘
                                             ▼ (3 enlaces BLE GATT directos: 0xFF10)
                         ┌───────────────────────────────────────┐
                         │   Maestro / Gateway / Border Router   │
                         │     ESP32-S3 N16R8 (formato UNO)      │
                         │ - Central BLE GATT (NimBLE)           │
                         │ - Coordinador N1↔N2 + FSM Global      │
                         │ - Wi-Fi (dual failover) + Bot Telegram│
                         └───────────────────┬───────────────────┘
                                             │ Wi-Fi (HTTPS / Long Polling)
                                             ▼
                         ┌───────────────────────────────────────┐
                         │           Grupo de Telegram           │
                         │  Alertas, /panel (Inline) y Comandos  │
                         └───────────────────────────────────────┘
```

### Justificación de la topología en estrella sobre BLE GATT (NimBLE)

Al contar con un **ESP32-S3 N16R8** con radio **BLE 5.0 + Wi-Fi** y 8 MB de Octal PSRAM:
1. La conexión directa **1 Maestro (S3) ↔ 3 Periféricos (C6)** mediante **NimBLE GATT** elimina la complejidad de aprovisionamiento Mesh, reduce consumo de flash/RAM en todas las placas y evita tráfico de inundación (*Relay*), mejorando la coexistencia **BLE + Wi-Fi** en el ESP32-S3.
2. Las **3 ESP32-C6** dedican el 100 % de su radio a un enlace punto a punto directo con el Maestro, manteniendo tiempos de respuesta deterministas.
3. El Maestro S3 enruta al instante los eventos de acceso del **Nodo 1** hacia la alarma del **Nodo 2** en la estrella local, operando normalmente aunque no haya Wi-Fi ni Internet.

### Distribución confirmada de placas y periféricos

| Placa | Modelo exacto | Periféricos asignados | Responsabilidad principal |
| :--- | :--- | :--- | :--- |
| **Nodo 1** | `ESP32-C6-DevKitC-1` | **RFID-RC522** (SPI, 3.3V) + **Cerradura** (LED toggle) + RGB integrado (`GPIO 8`) | Validación de UIDs desde NVS local, enclavamiento de cerradura, gestión remota `/rfid` y envío de eventos/comandos de armado/desarme vía Maestro S3. |
| **Nodo 2** | `ESP32-C6-DevKitC-1` | **PIR HC-SR501** (5V alim., 3.3V señal) + **Alarma tipo carro** (LED, sin buzzer) + RGB (`GPIO 8`) | Detección de movimiento por interrupción (`ISR → TaskNotify`), patrones de alarma automotriz y disparo por PIR o 3 intentos RFID inválidos. |
| **Nodo 3** | `ESP32-C6 SuperMini` | **DHT11** (bus 1-wire vía **RMT**, 3.3V) + RGB integrado (`GPIO 8`) | Lectura periódica no bloqueante cada 3 s y **alertas por sobresalto térmico** (`|ΔT| >= 2.0 °C` y umbral `>= 30 °C` con histéresis de `1.0 °C`). |
| **Gateway** | `ESP32-S3 N16R8` (formato UNO) | Sin sensores externos + botón `BOOT` (`GPIO 0`) + RGB integrado | Maestro BLE GATT (3 conexiones simultáneas), coordinador local N1 ↔ N2, failover dual Wi-Fi, cliente HTTPS Telegram (`/panel`) y recolector de métricas STR. |

> **Nota sobre ESP32-S3 N16R8 UNO:** habilitar `CONFIG_SPIRAM=y` y `CONFIG_SPIRAM_MODE_OCT=y` para alojar buffers mbedTLS/HTTP/JSON en PSRAM (manteniendo stacks de tareas críticas en SRAM interna), y evitar los pines reservados **`GPIO 33–37`** (Octal PSRAM), **`GPIO 26–32`** (Flash) y **`GPIO 19–20`** (USB nativo).

---

## 3. Justificación del Stack: ESP-IDF puro vs. Zephyr

Se confirma **ESP-IDF puro (v6.1.1) + FreeRTOS** frente a Zephyr RTOS por razones de riesgo técnico y rigor en Sistemas en Tiempo Real:

| Criterio | ESP-IDF puro (v6.1.1 + FreeRTOS) | Zephyr RTOS |
| :--- | :--- | :--- |
| **Coexistencia Wi-Fi + BLE (NimBLE)** | Soporte oficial maduro (`NimBLE` + árbitro `esp_coex` validado en ESP32-S3). | Alta fricción para Wi-Fi + BLE simultáneos sobre blobs binarios de Espressif. |
| **Periféricos clave (RMT, Octal PSRAM, TWDT)** | Drivers nativos listos para RMT (DHT11 no bloqueante en C6) y PSRAM Octal (`R8`) en S3. | Configuraciones complejas de *Devicetree overlays* y soporte desigual de periféricos propios. |
| **Sustentación académica STR** | Expone directamente **Fixed-Priority Preemptive Scheduling**, herencia de prioridad en Mutex, `xTaskNotifyFromISR`, **Core Pinning** SMP y `vTaskGetRunTimeStats`. | Su principal fortaleza (portabilidad entre múltiples fabricantes como Nordic/STM32) no aporta valor en un sistema 100 % Espressif. |

---

## 4. Red BLE GATT en Estrella: Conexión y Protocolo

- **Descubrimiento y reconexión automática (NimBLE Central en ESP32-S3):** cada C6 anuncia su nombre fijo (`IOT-N1-ACCESO`, `IOT-N2-SEGURIDAD`, `IOT-N3-CLIMA`) exponiendo el servicio GATT **`0xFF10`** con dos características: **`0xFF11` (`Notify`, C6 → S3)** y **`0xFF12` (`Write with Response`, S3 → C6)**. El Maestro S3 escanea continuamente cuando falta algún periférico, se conecta, descubre el servicio y activa notificaciones sin intervención manual.
- **Protocolo binario compacto (`components/ble_protocol/`, opcodes `0xC0..0xCE`):**
  - Se reutilizan las mismas tramas empaquetadas (`__attribute__((packed))`) sobre las características GATT para acceso (`OP_LOCK_*`, `OP_RFID_*`), seguridad (`OP_ALARM_*`), alertas térmicas (`OP_CLIMA_TELEMETRY`, `OP_THRESHOLD_SET`), salud (`OP_HEARTBEAT` cada 15 s) y métricas (`OP_METRICS_REPORT`).
  - **Enrutamiento local N1 → S3 → N2:** cuando Nodo 1 lee una tarjeta RFID autorizada, alterna su cerradura y notifica `OP_ALARM_SET` al Maestro S3, que lo reenvía de inmediato por GATT Write al Nodo 2 para armar/desarmar la alarma aunque no exista Internet.

---

## 5. Lógica de Alarma ("tipo carro"), Alertas Térmicas y Bot de Telegram

- **FSM de Alarma (3 estados exactos, armado instantáneo sin tiempo de gracia):**
  - **`DESARMADA`**: confirmada con **1 destello** al entrar; LED apagado.
  - **`ARMADA`**: confirmada al instante con **doble destello rápido**; armada por `/armar` o al cerrar la cerradura con RFID.
  - **`DISPARADA`**: activada al instante por **PIR** o tras **3 intentos consecutivos de RFID no autorizado**; **parpadeo rápido continuo indefinido** + alerta urgente en Telegram hasta `/desarmar` o tarjeta RFID válida.
- **Cerradura (Nodo 1):** modo **enclavado (*toggle*)** en `GPIO 20`; tarjeta autorizada alterna `ABIERTA`/`CERRADA` y arrastra a la alarma (estilo carro vía S3), mientras `/cerradura abrir|cerrar` en Telegram controla únicamente la cerradura.
- **Alertas por sobresalto de temperatura (Nodo 3 — DHT11 RMT):**
  - **Sobresalto brusco:** dispara alerta inmediata si **`|ΔT| >= 2.0 °C`** entre lecturas consecutivas.
  - **Umbral superior:** dispara alerta si **`T >= T_high`** (defecto **`30 °C`**, ajustable con `/umbral <°C>` en rango `15..50 °C` y guardado en NVS), con **histéresis de `1.0 °C`** al bajar (`T < T_high - 1.0 °C`).
  - Notifica al grupo solo ante sobresaltos, cruces de umbral o bajo consulta `/estado` (sin spam periódico).
- **Bot de Telegram en grupo autorizado:**
  - Comandos de texto (`/panel`, `/estado`, `/armar`, `/desarmar`, `/cerradura`, `/umbral`, `/rfid`, `/red`, `/metricas`) + **botones interactivos (*Inline Keyboard* en `/panel`)**.
  - Acceso verificado por `chat_id` del grupo (cualquier miembro del grupo opera); *Group Privacy* desactivado en `@BotFather`.
  - Credenciales en `secrets.h` (ignorado por Git, con plantilla `secrets.example.h` versionada) soportando **2 redes Wi-Fi con failover automático** (casa + hotspot móvil).

---

## 6. Documentación Técnica y Replicación (`docs/` y `REQUIREMENTS.md`)

Las especificaciones detalladas de arquitectura, pinout, protocolo binario, lógica funcional, entorno de desarrollo y requerimientos se encuentran en:

- [**Requerimientos y Guía de Replicación (`REQUIREMENTS.md`)**](REQUIREMENTS.md) — Requisitos agnósticos de software (Linux/Windows/macOS), BOM completo con alternativas de hardware y niveles de replicación sin depender de placas físicas.
- [**Entorno de Desarrollo en Linux (`ESP-IDF v6.1.1`)**](docs/entorno_desarrollo.md) — Instalación reproducible en Fedora 45 con Python 3.13 aislado, verificación agnóstica de permisos seriales/`udev` y flujo diario por terminal.
- [**Pinout y Especificaciones de Hardware**](docs/hardware/pinout.md) — Mapeo GPIO de las 4 placas (`2× C6-DevKitC-1`, `C6 SuperMini`, `S3 N16R8 UNO`), restricciones de pines, alimentación y código RGB de estado.
- [**Protocolo BLE GATT en Estrella y Tramas Binarias**](docs/diagramas/protocolo_ble_gatt.md) — Advertising de periféricos, servicio GATT `0xFF10`, reconexión del Maestro S3, opcodes y payloads byte a byte.
- [**Máquinas de Estados (FSM) y Bot de Telegram**](docs/diagramas/fsm_y_telegram.md) — Transiciones de alarma tipo carro, toggle de cerradura, gestión NVS `/rfid`, alertas por sobresalto térmico, diseño del `/panel` *Inline Keyboard* y failover Wi-Fi.
- [**Diseño Formal RTOS y Plan Experimental STR**](docs/metricas-str/diseno_rtos_str.md) — Sustentación FreeRTOS vs. Zephyr, tablas formales de tareas/prioridades/deadlines, *Core Pinning*, TWDT e instrumentación por software (`esp_timer_get_time()` + `/metricas`).
- [**Bitácora de Pruebas y Resultados Experimentales**](docs/metricas-str/bitacora_pruebas.md) — Registro cronológico de validaciones en hardware real, métricas STR observadas y estado de avance por nodo.

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
│   │   ├── protocolo_ble_gatt.md             # Contrato BLE GATT estrella y tramas binarias
│   │   └── fsm_y_telegram.md                 # FSM alarma/cerradura, alertas T y UI Telegram
│   ├── hardware/
│   │   └── pinout.md                         # Pinout de las 4 placas y código LED RGB
│   └── metricas-str/
│       ├── diseno_rtos_str.md                # Tablas RTOS, deadlines, FreeRTOS vs Zephyr y E1–E6
│       └── bitacora_pruebas.md               # Registro de pruebas en hardware y métricas reales
├── components/
│   ├── common_types/                         # Estructuras compartidas, LED RGB, métricas STR y BOOT
│   └── ble_protocol/                        # Constantes GATT estrella, opcodes y payloads packed
├── firmware/
│   ├── nodo1_acceso_c6/                      # Proyecto ESP-IDF (esp32c6 DevKitC-1): RC522 + Cerradura
│   ├── nodo2_seguridad_c6/                   # Proyecto ESP-IDF (esp32c6 DevKitC-1): PIR + Alarma
│   ├── nodo3_clima_c6/                       # Proyecto ESP-IDF (esp32c6 SuperMini): DHT11 (alertas T)
│   └── gateway_s3_uno/                       # Proyecto ESP-IDF (esp32s3 N16R8 UNO): Maestro BLE + Telegram
└── tools/                                    # Scripts Python para procesar logs [STR_METRIC]
```

---

## 8. Plan de Acción por Fases

- [x] **Fase 1 — Pinout, contratos de protocolo y diseño RTOS**
  - Definición de hardware (`2× C6-DevKitC-1`, `C6 SuperMini`, `S3 N16R8 UNO`), protocolo binario, FSMs, interfaz Telegram `/panel` y tablas formales STR documentadas en `docs/`.
- [x] **Fase 2 — Drivers y pruebas unitarias FreeRTOS en las 3 ESP32-C6** *(Completada: los 3 nodos validados en placa)*
  - Componentes `common_types` + `ble_protocol` y firmwares standalone para Nodo 1 (RC522 + NVS + cerradura), Nodo 2 (PIR + alarma) y Nodo 3 (DHT11 RMT + monitor de sobresalto térmico).
- [x] **Fase 3 — Red BLE GATT en estrella (NimBLE: 1 Maestro S3 ↔ 3 Periféricos C6)** *(Completada: estrella 3/3, reconexión automática y ruta offline N1→S3→N2 validados en 4 placas)*
  - Crear proyecto `gateway_s3_uno` como Central/Maestro BLE GATT, añadir servicio GATT (`0xFF10`) en las 3 C6, validar reconexión automática y enrutamiento local N1 → S3 → N2.
- [x] **Fase 4 — Coexistencia Wi-Fi + BLE y Bot de Telegram en el ESP32-S3** *(Completada: Octal PSRAM, `esp_coex`, failover dual Wi-Fi y Bot de Telegram `/panel` validados en placa)*
  - Configurar Octal PSRAM (`N16R8`), `esp_coex`, *Core Pinning* (Core 0/Core 1), failover dual Wi-Fi (`secrets.h`), *Long Polling* HTTPS (`keep-alive`), comandos de texto y `/panel` *Inline Keyboard*.
- [x] **Fase 5 — Lógica integral, gestión `/rfid` y pruebas en modo offline/degradado** *(Completada: flujo integral, `/rfid` NVS, `/umbral`, sobresalto térmico, failover dual y reconexión `3/3` validados en placa)*
  - Validar flujo completo de alarma (incluidos 3 fallos RFID), alertas por sobresalto de temperatura, alta/baja remota de tarjetas en NVS y operación en estrella sin Internet.
- [x] **Fase 6 — Mediciones de Tiempo Real (E1–E6) y documentación de resultados** *(Completada: herramienta `tools/analyze_str_metrics.py`, matriz `E1–E6` medida en hardware real y resultados consolidados en `docs/metricas-str/bitacora_pruebas.md`)*
  - Recolectar logs `[STR_METRIC]`, procesar percentiles/WCRT/jitter en `tools/`, registrar resultados de coexistencia en `docs/metricas-str/` y consolidar entregas de **IoT** y **STR**.

---

## 9. Enfoque Comercial: De Prototipo Académico a Producto Vendible

La arquitectura desarrollada puede empaquetarse como un producto comercial bajo el concepto de **Sistema Modular de Seguridad, Acceso y Monitoreo Térmico Autónomo (Edge-First IoT)**.

### Propuesta de valor
> *"Control de acceso RFID, alarma inteligente y alerta térmica temprana en tiempo real desde Telegram, sin cuotas mensuales, sin depender de servidores externos y con operación local garantizada aunque se caiga el Internet."*

### Segmentos objetivo
- **Apartamentos, habitaciones en arriendo y hogares:** armado/desarmado tipo carro al entrar o salir con llavero RFID, sin llaves duplicables fácilmente y con alertas inmediatas al grupo familiar en Telegram.
- **Consultorios, oficinas pequeñas y comercios:** gestión remota de acceso (`/rfid add|del`) para empleados temporales sin cambiar cerraduras ni pagar software en la nube.
- **Cuartos técnicos, racks de servidores o bodegas:** detección de intrusión + detección temprana de fallas térmicas por gradiente (`|ΔT| >= 2.0 °C`) antes de que el equipo alcance temperaturas críticas.

### Ventajas competitivas frente a soluciones comerciales típicas
| Diferenciador | Soluciones Wi-Fi económicas (nube propietaria) | Este sistema (BLE GATT Estrella + Border Router) |
| :--- | :--- | :--- |
| **Autonomía sin Internet** | Muchas pierden automatizaciones o acceso remoto si cae la nube. | Enrutamiento local `Acceso → Gateway → Alarma` en **`< 5 ms`** sin Wi-Fi ni Internet. |
| **Costos recurrentes / App** | Apps cerradas, dependencia de servidores externos o suscripciones. | Interfaz nativa en **Telegram** (`/panel` interactivo) sin servidor intermedio ni mensualidades. |
| **Resiliencia de enlace** | Un solo Wi-Fi; si falla el router queda incomunicado. | **Failover dual automático** (Wi-Fi principal + respaldo móvil/4G). |
| **Alerta térmica inteligente** | Solo umbral fijo simple. | **Sobresalto brusco (`|ΔT| >= 2 °C`) + umbral dinámico con histéresis (`1 °C`)**. |

### Hoja de ruta de industrialización (para llevarlo a venta)
1. **Hardware y encapsulado:** diseñar PCBs compactas (reemplazando protoboards), gabinetes impresos en 3D o riel DIN, y etapa de potencia optoacoplada/MOSFET para accionar cerradura solenoide/electroimán real y sirena acústica.
2. **Provisionamiento sin código (UX cliente final):** reemplazar `secrets.h` por un portal cautivo inicial (SoftAP web o *BLE Provisioning*) para que el usuario ingrese su Wi-Fi, token y grupo desde el celular en 2 minutos.
3. **Seguridad y mantenimiento en campo:** habilitar cifrado/emparejamiento BLE (*NimBLE Bonding AES-CCM*) y actualizaciones **OTA** firmadas sobre HTTPS.
4. **Respaldo eléctrico (UPS 5V):** integrar batería Li-Ion con cargador y conmutación automática para operar durante cortes de energía eléctrica.
