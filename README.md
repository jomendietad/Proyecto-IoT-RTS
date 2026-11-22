# Sistema Distribuido IoT y de Tiempo Real para Automatización y Seguridad de Habitación (BLE Mesh + Border Router Telegram)

Proyecto académico integrador diseñado para cumplir simultáneamente los requerimientos técnicos de las asignaturas **Desarrollo de Sistemas IoT** y **Sistemas en Tiempo Real (STR)**.

El sistema implementa una red mallada inalámbrica basada en **Bluetooth Low Energy Mesh (BLE Mesh)** sobre **3 nodos ESP32-C6** y **1 nodo central ESP32-S3 N16R8 (formato UNO)** que actúa como **Provisioner BLE Mesh, Gateway y Border Router Wi-Fi** hacia Internet mediante un **Bot de Telegram** operando en un grupo.

---

## 1. Resumen del Proyecto y Objetivos Académicos (IoT + STR)

El objetivo es monitorear, asegurar y automatizar una habitación mediante un sistema distribuido con **4 microcontroladores** que operan bajo restricciones temporales definidas, mantienen **autonomía local ante fallos de conectividad** (*offline-first* en la malla) y exponen telemetría y telecontrol bidireccional por Internet.

| Dimensión | Aportes principales del proyecto |
| :--- | :--- |
| **Desarrollo de Sistemas IoT** | • Red mallada **BLE Mesh (ESP-BLE-MESH)** con enrutamiento multi-salto (*Relay*) y aprovisionamiento automático sin app móvil.<br>• Arquitectura **Gateway / Border Router** desacoplada (BLE Mesh ↔ Wi-Fi / HTTPS TLS ↔ Telegram Bot API).<br>• Interacción directa **M2M (C6 ↔ C6)** mediante modelo *Publish/Subscribe* por grupos.<br>• Resiliencia y seguridad básica: persistencia en NVS, validación estricta de `chat_id` del grupo autorizado en Telegram, reconexión automática Wi-Fi/TLS y monitoreo mediante *heartbeats*. |
| **Sistemas en Tiempo Real (STR)** | • Diseño concurrente sobre **ESP-IDF puro + FreeRTOS** sin esperas activas ni `delay()` bloqueantes en rutas críticas.<br>• Planificación por prioridades (Firm/Hard RT local para interrupción PIR, RFID y alarma; Soft RT para DHT11 y *long polling* HTTPS).<br>• Uso explícito de primitivas RTOS: `ISR → xTaskNotifyFromISR`, `xQueue`, `EventGroups`, `Mutex`, `Software Timers` (`xTimer`) y **Task Watchdog Timer (TWDT)**.<br>• Aislamiento por núcleos (**Core Pinning** SMP) en el ESP32-S3 para separar stacks de radio/coexistencia de la lógica de aplicación y Telegram.<br>• Lectura determinista del DHT11 usando periférico hardware **RMT** en lugar de *bit-banging* con bloqueo de interrupciones.<br>• Instrumentación y evaluación experimental de latencias (WCRT), *jitter*, consumo de pila (*high-water mark*) e impacto de la coexistencia Wi-Fi + BLE. |

---

## 2. Arquitectura del Sistema (4 placas: 3× ESP32-C6 + 1× ESP32-S3 N16R8 UNO)

### Diagrama general

```text
               [ Red BLE Mesh (ESP-BLE-MESH) — 4 nodos ]

  ┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
  │ Nodo 1 (ESP32-C6)   │   │ Nodo 2 (ESP32-C6)   │   │ Nodo 3 (ESP32-C6)   │
  │ Acceso              │◄─►│ Seguridad / Alarma  │◄─►│ Climatización       │
  │ - RFID-RC522 (SPI)  │   │ - PIR HC-SR501      │   │ - DHT11 (RMT)       │
  │ - Cerradura (LED)   │   │ - Alarma (LED/patr.)│   │ - Ventilador (LED)  │
  │   [Relay activo]    │   │   [Relay activo]    │   │   [Relay activo]    │
  └──────────┬──────────┘   └──────────┬──────────┘   └──────────┬──────────┘
             │                         │                         │
             └─────────────────────────┼─────────────────────────┘
                                       ▼ (BLE Mesh directo, 1 o más saltos)
                         ┌───────────────────────────────────┐
                         │    Gateway / Border Router        │
                         │   ESP32-S3 N16R8 (formato UNO)    │
                         │ - Provisioner BLE Mesh [NO-Relay] │
                         │ - Coordinador + FSM Global        │
                         │ - Wi-Fi + TLS + Bot de Telegram   │
                         └─────────────────┬─────────────────┘
                                           │ Wi-Fi (HTTPS / Long Polling)
                                           ▼
                         ┌───────────────────────────────────┐
                         │         Grupo de Telegram         │
                         │  Telemetría, Alertas y Comandos   │
                         └───────────────────────────────────┘
```

### Justificación técnica de eliminar el 4.º nodo RCP

En el ecosistema Espressif, el esquema **ESP32-C6 (RCP) + ESP32-S3 (Border Router)** es obligatorio cuando la red mallada utiliza protocolos **IEEE 802.15.4 (Thread o Zigbee)**, dado que el ESP32-S3 carece de radio 802.15.4. Sin embargo, dado que este proyecto utiliza **Bluetooth Low Energy (BLE Mesh)** y el **ESP32-S3 cuenta con radio BLE 5.0 nativa junto con Wi-Fi**, prescindir del RCP aporta beneficios directos:

1. **Menor latencia y menor complejidad:** se elimina un microcontrolador intermedio y el protocolo puente local por Wi-Fi/UART entre el C6 y el S3, reduciendo saltos de red y puntos de fallo.
2. **Mayor determinismo en las 3 ESP32-C6:** los nodos sensores/actuadores dedican el 100 % de su radio de 2.4 GHz a **BLE Mesh**, evitando contención con Wi-Fi en los nodos de campo.
3. **Aprovechamiento del doble núcleo y PSRAM del ESP32-S3:** el S3 centraliza BLE Mesh + Wi-Fi + mbedTLS/HTTPS contando con CPU dual-core a 240 MHz y 8 MB de PSRAM.

### Distribución confirmada de placas y periféricos

| Placa | Rol en la red | Periféricos asignados | Responsabilidad principal |
| :--- | :--- | :--- | :--- |
| **Nodo 1 (`ESP32-C6`)** | Acceso (*Relay*) | **RFID-RC522** (SPI, 3.3V) + **Cerradura emulada** (LED) | Lectura SPI, validación local de UIDs autorizados, pulso temporizado de apertura y publicación de eventos de acceso/desarme directo a la malla. |
| **Nodo 2 (`ESP32-C6`)** | Seguridad / Intrusión (*Relay*) | **PIR HC-SR501** (5V alim., 3.3V señal) + **Alarma tipo carro** (LED con patrones; opcional buzzer) | Detección de movimiento por interrupción GPIO, ejecución local determinista de patrones de alarma (`ARMADA`, `DISPARADA`, etc.) y alerta prioritaria. |
| **Nodo 3 (`ESP32-C6`)** | Climatización (*Relay*) | **DHT11** (bus 1-wire vía RMT) + **Ventilador DC emulado** (LED / PWM LEDC) | Muestreo periódico no bloqueante de temperatura/humedad y control manual o automático (por umbral con histéresis) del ventilador. |
| **Gateway (`ESP32-S3 N16R8 UNO`)** | Provisioner BLE Mesh + Border Router (*NO-Relay*) | Sin sensores de campo | Aprovisionamiento automático de nodos C6, mantenimiento de tabla de estado global, FSM coordinadora de alarma, enlace Wi-Fi y cliente HTTPS de Telegram. |

### Especificaciones y precauciones del ESP32-S3 N16R8 en formato UNO

La placa elegida cuenta con **16 MB de Flash (`N16`)** y **8 MB de Octal PSRAM (`R8`)**, lo cual resuelve el principal cuello de botella de ejecutar simultáneamente BLE Mesh, Wi-Fi y TLS/HTTPS (mbedTLS).

- **Lineamientos de configuración en ESP-IDF:**
  - Habilitar PSRAM en modo Octal (`CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`).
  - Configurar mbedTLS y buffers HTTP/JSON dinámicos para alojarse en PSRAM, manteniendo **stacks de tareas críticas, colas e ISRs en SRAM interna** para preservar el determinismo en Tiempo Real.
- **Precauciones de hardware por formato UNO y Octal PSRAM:**
  - Verificar la equivalencia exacta entre la **serigrafía tipo Arduino UNO (`D0..D13`, `A0..A5`)** del fabricante de la placa y los **GPIOs nativos del ESP32-S3** antes de asignar cualquier pin auxiliar (p. ej. LED de estado o botón físico).
  - **No utilizar los GPIOs 33 al 37**, ya que están reservados internamente para el bus de la **Octal PSRAM**.
  - Si se utiliza la consola por USB nativo (USB-CDC / JTAG), **no reutilizar los GPIOs 19 y 20**.

### Notas rápidas de hardware y cableado para los nodos ESP32-C6

- **RFID-RC522 (Nodo 1):** alimentar **estrictamente a 3.3V** (no es tolerante a 5V en alimentación ni recomendado en lógica). Compartir masa (`GND`) y usar pines SPI dedicados con resistencia o línea estable en `CS` y `RST`.
- **PIR HC-SR501 (Nodo 2):** requiere **5V en `VCC`** para estabilizar su regulador interno (o puente según módulo), mientras que su salida digital `OUT` entrega **3.3V** (compatible con ESP32-C6). Ajustar potenciómetros de sensibilidad/tiempo al mínimo útil y definir el jumper físico (*Repeatable trigger* `H` vs. *Single trigger* `L`) antes de medir tiempos de respuesta; contemplar el tiempo inicial de estabilización del sensor al arrancar (~30–60 s en hardware real o filtrado por software al iniciar).
- **DHT11 (Nodo 3):** usar resistencia *pull-up* en la línea de datos (4.7 kΩ–10 kΩ a 3.3V si el módulo PCB no la incluye) y respetar el intervalo mínimo de muestreo del sensor (≥ 2 s; recomendado 2–5 s).
- **LEDs actuadores:** emplear siempre resistencias limitadoras adecuadas (220 Ω–330 Ω) acorde a la corriente máxima por GPIO de la ESP32-C6.

---

## 3. Justificación del Stack: ESP-IDF puro vs. Zephyr

Se **confirma el uso de ESP-IDF puro (v5.x) + FreeRTOS** como la mejor decisión técnica y de ingeniería de riesgos frente a Zephyr RTOS para los requisitos específicos de este proyecto.

| Criterio técnico | ESP-IDF puro (v5.x + FreeRTOS) | Zephyr RTOS |
| :--- | :--- | :--- |
| **Coexistencia Wi-Fi + BLE Mesh en ESP32-S3** | Soporte oficial de primer nivel mediante `esp_ble_mesh` + módulo de coexistencia `esp_coex` validado por Espressif para radio compartida de 2.4 GHz. | Soporte de Wi-Fi + BLE simultáneos en SoCs Espressif (especialmente S3/C6) con mayor fricción de integración, parches de blobs binarios y menor madurez en ejemplos combinados de BLE Mesh + TLS. |
| **Soporte de periféricos específicos** | Drivers nativos y estables para **Octal PSRAM** en S3 N16R8, periférico **RMT** (clave para leer DHT11 sin bloquear la CPU en C6), SPI y **TWDT**. | Requiere configurar *Devicetree overlays* y Kconfig complejos; varios periféricos específicos (como RMT para DHT11 o ajustes finos de Octal PSRAM) demandan trabajo extra de portabilidad. |
| **Valor académico en Sistemas en Tiempo Real (STR)** | Expone directamente primitivas estándar de **FreeRTOS** (`xTaskCreatePinnedToCore`, `xTaskNotifyFromISR`, `xQueue`, `Semaphore/Mutex`, `xTimer`, análisis de prioridades y *watermark*), muy claras para documentar y sustentar. | Abstracción propia de kernel excelente, pero añade curva de aprendizaje en Kconfig/Devicetree/West que desvía tiempo del diseño RTOS y de la lógica distribuida. |
| **Homogeneidad de hardware** | Las 4 placas son 100 % Espressif (`3x ESP32-C6` + `1x ESP32-S3`), permitiendo compartir código en `components/` nativamente. | Su mayor ventaja (portabilidad multi-fabricante hacia Nordic/STM32/NXP) no se aprovecha al usar únicamente familia ESP32. |

**Conclusión:** usar **ESP-IDF puro** elimina dolores de cabeza en coexistencia Wi-Fi/BLE Mesh, PSRAM Octal y RMT, acelerando el desarrollo estable de ambos cursos.

---

## 4. Red BLE Mesh: Aprovisionamiento Automático e Interacción Directa C6 ↔ C6

### Aprovisionamiento automático (Provisioner en ESP32-S3)

Para evitar depender de una aplicación móvil externa (*nRF Mesh*) en demostraciones o reinicios, el **ESP32-S3** operará como **Provisioner BLE Mesh** embebido mediante el portador **PB-ADV**:

1. **Identificación determinista por Device UUID (16 bytes):** cada nodo C6 anuncia un UUID con un prefijo fijo del proyecto (por ejemplo `0xAA, 0x55`) y un byte identificador de rol (`0x01` Acceso, `0x02` Seguridad, `0x03` Clima).
2. **Asignación automática de direcciones Unicast:** al detectar un nodo no aprovisionado con el prefijo válido, el S3 le asigna siempre su dirección lógica predefinida:
   - `0x0001`: **ESP32-S3** (Provisioner / Gateway)
   - `0x0010`: **Nodo 1** (Acceso — ESP32-C6)
   - `0x0020`: **Nodo 2** (Seguridad — ESP32-C6)
   - `0x0030`: **Nodo 3** (Climatización — ESP32-C6)
3. **Autoconfiguración post-aprovisionamiento:** mediante *Configuration Client*, el S3 distribuye automáticamente la `AppKey`, realiza el *Model App Bind* de los modelos y configura las direcciones de publicación y suscripción de cada nodo.
4. **Persistencia en NVS y recuperación:** activando `CONFIG_BLE_MESH_SETTINGS=y`, tanto el Provisioner como los nodos guardan claves, direcciones e *IV Index* en NVS. Al reiniciar por corte eléctrico, la red se recompone automáticamente sin re-aprovisionar. Adicionalmente, se implementará un **botón de *factory reset*** (p. ej. mantener presionado el botón `BOOT` durante 5 s) para limpiar NVS durante pruebas de laboratorio.

### Interacción directa entre nodos C6 (Publish/Subscribe) y tolerancia a fallos

Se definirá un **Vendor Model** propio en `components/mesh_protocol/` con tramas binarias compactas y esquema de **confirmaciones (ACK) / retransmisiones** para comandos críticos (cerradura y armado/desarmado de alarma).

#### Direcciones de grupo propuestas

| Dirección de Grupo | Nombre lógico | Quién publica | Quién se suscribe | Propósito |
| :--- | :--- | :--- | :--- | :--- |
| **`0xC000`** | Sistema / Broadcast | Nodos 1, 2, 3 y Gateway S3 | Gateway S3 (y nodos según comando global) | *Heartbeats*, consulta de estado general y sincronización. |
| **`0xC001`** | Seguridad y Alarma | Nodo 1 (RFID válido), Nodo 2 (PIR / estado alarma), Gateway S3 (comandos Telegram) | Nodo 2 (Alarma), Gateway S3 (y Nodo 1 opcional) | Desarme/armado directo desde RFID hacia alarma, disparos de intrusión y cambio de estado de alarma. |
| **`0xC002`** | Acceso | Nodo 1 (eventos RFID/cerradura), Gateway S3 (comandos Telegram) | Nodo 1, Gateway S3 | Apertura/cierre remoto y notificación de tarjetas aceptadas o rechazadas. |
| **`0xC003`** | Climatización | Nodo 3 (telemetría DHT11/ventilador), Gateway S3 (comandos Telegram) | Nodo 3, Gateway S3 | Lecturas de temperatura/humedad y control manual/automático del ventilador. |

#### Autonomía en modo degradado (sin Wi-Fi ni Internet)

Gracias a la suscripción directa en la malla:
- Si se cae el router Wi-Fi o se pierde acceso a Internet, una **tarjeta RFID autorizada en el Nodo 1** abre la cerradura localmente y publica un mensaje de desarme directo al grupo **`0xC001`**, haciendo que el **Nodo 2 desarme la alarma de inmediato** sin esperar al ESP32-S3 ni a Telegram.
- De igual forma, el **Nodo 2** sigue disparando la alarma local ante movimiento del PIR si está armada, y el **Nodo 3** mantiene la regulación automática del ventilador por umbral de temperatura.
- Al recuperarse la conexión Wi-Fi, el ESP32-S3 sincroniza el último estado conocido hacia el grupo de Telegram.

---

## 5. Lógica de Alarma ("tipo carro") e Integración con Bot de Telegram

### Máquina de Estados Finita (FSM) de la alarma

```text
                /armar (o RFID)
  [DESARMADA] ───────────────────► [ARMANDO] (gracia 5–10 s; parpadeo lento)
       ▲                                │
       │ /desarmar o                    │ fin temporizador
       │ RFID válido                    ▼
       ├─────────────────────────── [ARMADA] (confirmación doble destello)
       │                                │
       │ /desarmar o                    │ PIR detecta movimiento (o RFID no autoriz.)
       │ RFID válido                    ▼
       └─────────────────────────── [DISPARADA] (parpadeo rápido + alerta Telegram)
```

- **`DESARMADA`:** el sistema de intrusión no dispara alerta; la cerradura funciona normalmente vía RFID o Telegram.
- **`ARMANDO`:** estado transitorio de salida (ej. 5–10 s) con destello lento del LED indicador para permitir abandonar la habitación sin auto-dispararse.
- **`ARMADA`:** confirma activación con **doble destello rápido** (estilo alarma automotriz). Monitorea el evento de interrupción del PIR (y opcionalmente intentos reiterados de RFID inválido).
- **`DISPARADA`:** ejecuta un patrón intermitente rápido continuo mediante temporizador RTOS y encola una alerta de **máxima prioridad** hacia el grupo de Telegram. Solo regresa a `DESARMADA` mediante el comando `/desarmar` o una tarjeta RFID autorizada.

### Interacción en el Grupo de Telegram

- **Alertas y telemetría automáticas hacia el grupo:**
  - Acceso concedido o denegado por RFID (incluyendo UID y hora/secuencia).
  - Transición de estado de alarma (`ARMANDO`, `ARMADA`, `DISPARADA` por PIR, `DESARMADA` por RFID/Telegram).
  - Lecturas de temperatura y humedad (por cambio significativo, superación de umbral o bajo demanda).
  - Aviso de desconexión/reincorporación de algún nodo ESP32-C6 (*heartbeat* vencido).
- **Comandos soportados desde el grupo:**
  - `/estado` — reporte consolidado de sensores, actuadores, estado de alarma y nodos activos en la malla.
  - `/armar` y `/desarmar` — control de la alarma tipo carro.
  - `/cerradura abrir` / `/cerradura cerrar` — control remoto de la cerradura emulada en Nodo 1.
  - `/ventilador on` / `/ventilador off` / `/ventilador auto` — control manual o termostato automático en Nodo 3.
  - `/umbral <°C>` *(opcional)* — ajuste dinámico del umbral de encendido automático del ventilador.
- **Seguridad y operación de la API:**
  - Configurar el bot con `@BotFather` desactivando *Group Privacy* (o usando comandos explícitos `/comando@NombreBot`).
  - Validar obligatoriamente en el ESP32-S3 que cada actualización recibida pertenezca al **`chat_id` autorizado** del grupo, descartando mensajes de otros chats.
  - Usar **HTTPS *Long Polling* (`getUpdates`)** con sesión persistente (`keep-alive`) para evitar bloqueos detrás de NAT y reducir la sobrecarga de *handshakes* TLS.
  - Separar recepción de comandos y envío de notificaciones mediante una **cola priorizada** en el S3 (las alertas de alarma o intrusión adelantan a la telemetría rutinaria de temperatura).

---

## 6. Diseño de Sistemas en Tiempo Real (STR / FreeRTOS)

### Clasificación de tareas y primitivas RTOS

| Nodo | Tarea / ISR | Criticidad RT | Mecanismo de activación / IPC | Prioridad relativa |
| :--- | :--- | :--- | :--- | :--- |
| **Nodo 1 (C6)** | `task_rfid_poll` + `timer_lock` | Alta / Media | Polling rápido SPI (con `Mutex` de bus si aplica) + `xTimer` para pulso de cerradura | Alta / Media |
| **Nodo 1 (C6)** | `task_mesh_nodo1` | Media | Callbacks BLE Mesh ↔ `xQueue` | Media |
| **Nodo 2 (C6)** | `pir_gpio_isr` + `task_alarm_fsm` | **Alta (Firm RT local)** | Interrupción de flanco GPIO → `xTaskNotifyFromISR` + `xTimer` para patrón LED | **Muy Alta** |
| **Nodo 2 (C6)** | `task_mesh_nodo2` | Media | `xQueue` de eventos urgentes hacia malla | Media-Alta |
| **Nodo 3 (C6)** | `task_dht11_rmt` | Baja (Soft RT) | Temporizador periódico (2–5 s) usando canal hardware **RMT** (no bloqueante) | Baja |
| **Nodo 3 (C6)** | `task_fan_ctrl` | Media | `xQueue` / notificación de nueva lectura o comando remoto (histéresis) | Media |
| **Todos (C6 + S3)** | `TWDT` + `task_heartbeat` | Supervisión | Task Watchdog Timer de ESP-IDF + `xTimer` de *heartbeat* | Baja / Sistema |
| **Gateway (S3)** | Stacks BLE Mesh + Wi-Fi + LwIP | Alta (Sistema / Radio) | Gestor de coexistencia `esp_coex` fijado en **Core 0** | Sistema / Alta |
| **Gateway (S3)** | `task_state_fsm` | Media-Alta | `xQueue` unificada + `Mutex` de estado global en **Core 1** | Media-Alta |
| **Gateway (S3)** | `task_tg_tx` (Alertas/Telemetría) | Media | `xQueue` priorizada (alertas antes que telemetría) en **Core 1** | Media |
| **Gateway (S3)** | `task_tg_rx` (*Long Polling*) | Baja (Soft RT) | Ciclo HTTPS `getUpdates` con `keep-alive` en **Core 1** | Baja |

### Core Pinning en ESP32-S3 y estrategia de coexistencia

Para proteger el determinismo del **ESP32-S3 N16R8**, se emplea asignación explícita de núcleos (`xTaskCreatePinnedToCore`):
- **Core 0 (Protocolos / Radio):** tareas de Bluetooth/BLE Mesh, controlador Wi-Fi, pila LwIP y árbitro de coexistencia software (`esp_coex`).
- **Core 1 (Aplicación / Red superior):** máquina de estados global, tabla de estado de nodos, cola de prioridad y clientes HTTPS RX/TX hacia la API de Telegram.

**Mitigación de contención BLE Mesh + Wi-Fi en la radio única del S3:**
1. Configurar el ESP32-S3 con la característica **Relay deshabilitada (*NO-Relay*)** en BLE Mesh (los 3 nodos ESP32-C6 son quienes hacen de *Relay*), evitando que el S3 gaste ventanas de radio reenviando tramas ajenas.
2. Utilizar **mensajes con ACK/retransmisión** en el *Vendor Model* para eventos críticos (disparo PIR, apertura de cerradura, cambio de estado de alarma).
3. Mantener sesión **TLS persistente (`keep-alive`)** para Telegram alojando buffers pesados de mbedTLS en la **Octal PSRAM**.

### Métricas planificadas para evaluación STR (carpeta `docs/metricas-str/`)

1. **Latencia local (WCRT):** tiempo desde flanco de subida del PIR en Nodo 2 hasta activación del LED de alarma (objetivo: sub-milisegundo / pocos milisegundos).
2. **Latencia de red BLE Mesh:** tiempo desde evento en C6 hasta recepción en ESP32-S3 en **1 salto (directo)** vs. **2 saltos (vía Relay)**, comparando el escenario **sin tráfico Wi-Fi** vs. **con sesión HTTPS Telegram activa** (análisis experimental de coexistencia).
3. **Latencia extremo a extremo:** tiempo total desde detección física (PIR / RFID) hasta entrega del mensaje en el grupo de Telegram, y viceversa (comando Telegram → actuación física en C6).
4. **Jitter de tarea periódica:** desviación temporal en el muestreo periódico del DHT11 vía RMT.
5. **Uso de recursos y tolerancia a fallos:** *stack high-water mark* (`uxTaskGetStackHighWaterMark`) por tarea, ocupación de SRAM interna vs. Octal PSRAM y tiempo de recuperación tras inyección de bloqueo evaluado con **TWDT**.

---

## 7. Estructura del Repositorio

El repositorio está organizado como un monorepo con **componentes compartidos** y **4 proyectos independientes de ESP-IDF** (3 compilados para `target esp32c6` y 1 para `target esp32s3`), enlazando la carpeta `components/` desde cada proyecto mediante `EXTRA_COMPONENT_DIRS`:

```text
Proyecto-IoT-RTS/
├── README.md                           # Documentación integral, arquitectura y plan de acción
├── .gitignore                          # Exclusiones de build, sdkconfig local y secretos
├── docs/
│   ├── diagramas/                      # Diagramas de arquitectura, FSM y secuencias
│   ├── hardware/                       # Pinouts de las 3x C6 y S3 N16R8 UNO, esquemáticos
│   └── metricas-str/                   # Tablas de latencias, jitter, memoria y pruebas RTOS
├── components/
│   ├── common_types/                   # Estructuras compartidas, estados de alarma, errores
│   └── mesh_protocol/                  # Definición del Vendor Model, opcodes y payloads binarios
├── firmware/
│   ├── nodo1_acceso_c6/                # Proyecto ESP-IDF (esp32c6): RFID-RC522 + Cerradura
│   ├── nodo2_seguridad_c6/             # Proyecto ESP-IDF (esp32c6): PIR HC-SR501 + Alarma
│   ├── nodo3_clima_c6/                 # Proyecto ESP-IDF (esp32c6): DHT11 (RMT) + Ventilador
│   └── gateway_s3_uno/                 # Proyecto ESP-IDF (esp32s3): Provisioner + Wi-Fi + Telegram
└── tools/                              # Scripts auxiliares de análisis de logs y métricas
```

*(Los directorios vacíos contienen archivos `.gitkeep` temporales para preservar la estructura en Git hasta incorporar el código de cada módulo).*

---

## 8. Plan de Acción por Fases (Fases 1 a 6)

### Fase 1 — Pinout, contratos BLE Mesh y diseño RTOS
- Definir y documentar en `docs/hardware/` el mapeo exacto de pines de las **3 ESP32-C6** y del **ESP32-S3 N16R8 UNO** (verificando serigrafía UNO vs. GPIOs reservados de Octal PSRAM/USB).
- Especificar en `docs/diagramas/` el contrato del **Vendor Model** (opcodes, payloads, ACKs), el formato de Device UUID (`0xAA 0x55 ... <rol>`), las direcciones unicast/grupo y la tabla definitiva de prioridades FreeRTOS.
- **Criterio de validación:** mapa de pines libre de conflictos y especificación de tramas cerrada antes de escribir drivers.

### Fase 2 — Drivers y pruebas unitarias FreeRTOS en las 3 ESP32-C6
- Implementar y validar de forma aislada (sin red BLE Mesh) cada proyecto en `firmware/nodo*_c6/`, ya estructurado con tareas, colas, temporizadores y TWDT:
  - **Nodo 1:** driver SPI del RC522, lista de UIDs válidos y temporización no bloqueante de cerradura.
  - **Nodo 2:** interrupción de flanco del PIR (`ISR → TaskNotify`) + patrones LED de los 4 estados de alarma.
  - **Nodo 3:** lectura del DHT11 por periférico hardware **RMT** + lógica de control del ventilador con histéresis.
- **Criterio de validación:** cada nodo responde sin bloqueos, sin advertencias del Watchdog y con márgenes de stack verificados.

### Fase 3 — Red BLE Mesh completa (aprovisionamiento automático, C6 ↔ C6 y multi-salto)
- Implementar el **Provisioner automático** (PB-ADV) en `firmware/gateway_s3_uno/` y habilitar la persistencia NVS + *factory reset* por botón `BOOT` (5 s) en las 4 placas.
- Validar la comunicación bidireccional de modelos compartidos (`components/mesh_protocol/`):
  - *Uplink:* eventos RFID, disparos PIR, lecturas DHT11 y *heartbeats* hacia el S3.
  - *Downlink:* comandos desde el S3 hacia los nodos C6.
  - *Directo C6 ↔ C6:* desarme inmediato de alarma en Nodo 2 al leer tarjeta válida en Nodo 1 (grupo `0xC001`).
- Realizar prueba física de **enrutamiento multi-salto (*Relay*)** alejando o atenuando un nodo para confirmar que alcanza al S3 a través de otro C6.
- **Criterio de validación:** la malla se aprovisiona sola al encender, sobrevive a reinicios eléctricos (NVS) y confirma entrega multi-salto.

### Fase 4 — Coexistencia Wi-Fi + BLE Mesh y Bot de Telegram en el ESP32-S3
- Configurar en `gateway_s3_uno` la **Octal PSRAM (`N16R8`)**, la coexistencia Wi-Fi + BLE Mesh (`esp_coex`), el S3 como **NO-Relay** y el *Core Pinning* (Core 0 radio / Core 1 lógica y Telegram).
- Crear el bot en `@BotFather`, integrarlo en el grupo de pruebas, restringir acceso por `chat_id` e implementar `task_tg_rx` (*Long Polling* HTTPS con `keep-alive`) y `task_tg_tx` (cola priorizada).
- **Criterio de validación:** operación continua simultánea de BLE Mesh y HTTPS Telegram sin agotamiento de SRAM interna ni caída de enlaces.

### Fase 5 — Lógica integral de alarma tipo carro y modo offline/degradado
- Integrar la FSM completa de alarma sincronizada entre el Gateway S3, el grupo de Telegram y el Nodo 2 (incluyendo tiempo de gracia `ARMANDO`, doble destello en `ARMADA`, disparo por PIR y desarme por Telegram o RFID).
- Ejecutar pruebas de **tolerancia a fallos**:
  - Desconectar el router Wi-Fi / Internet y confirmar que el RFID sigue abriendo la cerradura y desarmando la alarma vía malla directa C6 ↔ C6, que el PIR sigue disparando la alarma local y que el ventilador continúa operando en modo automático.
  - Reconectar Wi-Fi y validar resincronización hacia Telegram.
- **Criterio de validación:** cero pérdida de funcionalidad crítica de seguridad local sin Internet y recuperación limpia al restaurar conectividad.

### Fase 6 — Mediciones de Tiempo Real, pruebas de estrés y documentación final
- Instrumentar tiempos (con `esp_timer_get_time()` / GPIO de traza) y registrar resultados en `docs/metricas-str/`:
  - Latencia PIR → alarma local.
  - Latencia BLE Mesh 1 salto vs. 2 saltos (con y sin tráfico HTTPS/Wi-Fi activo en S3).
  - Latencia extremo a extremo hasta Telegram.
  - Jitter del muestreo DHT11, *stack high-water mark*, consumo SRAM/PSRAM y pruebas de recuperación ante fallos forzados con TWDT.
- Consolidar diagramas definitivos en `docs/diagramas/` para las entregas e informes finales de **Desarrollo de Sistemas IoT** y **Sistemas en Tiempo Real**.
