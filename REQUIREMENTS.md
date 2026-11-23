# Requerimientos para Replicar el Proyecto (`REQUIREMENTS.md`)

Este documento especifica los requerimientos de **software**, **hardware**, **alternativas equivalentes** y **niveles de replicación** para que cualquier persona pueda compilar, probar o reproducir el proyecto independientemente de su sistema operativo, editor o proveedor exacto de placas.

---

## 1. Propósito y Alcance de Replicabilidad

El diseño modular del repositorio (`components/` desacoplado de `firmware/`) permite replicar el sistema en distintos grados:
- **Sin placas físicas:** compilando los 4 proyectos y verificando la lógica de negocio, FSMs y serialización.
- **Con hardware equivalente:** sustituyendo las tarjetas de desarrollo o sensores por modelos compatibles.
- **Sistema completo:** ejecutando la malla BLE Mesh de 4 nodos con Border Router Wi-Fi y Bot de Telegram.

---

## 2. Requerimientos de Software (Multiplataforma)

| Categoría | Requisito | Alternativas / Notas multiplataforma |
| :--- | :--- | :--- |
| **Sistema Operativo** | Linux (cualquier distro), Windows 10/11 o macOS | El flujo de compilación es idéntico en los tres sistemas |
| **Framework / SDK** | **ESP-IDF `v6.1.1`** (targets `esp32c6` y `esp32s3`) | Instalable vía **EIM** o vía Git clásico (`git clone -b v6.1.1 --recursive https://github.com/espressif/esp-idf.git` + `./install.sh esp32c6,esp32s3` / `install.ps1`) |
| **Python** | **Python `3.10 – 3.13`** (recomendado `3.13` aislado) | Evitar Python 3.15+ como intérprete del `venv` si EIM exige `<= 3.13` |
| **Herramientas de compilación** | `Git >= 2.30`, `CMake >= 3.22`, `Ninja`, `esptool >= 5.x` | `CMake`, `Ninja` y los toolchains (`riscv32-esp-elf`, `xtensa-esp-elf`) los provee el instalador de ESP-IDF; `ccache` es opcional para acelerar recompilaciones |
| **Drivers USB-Serial** | • **Linux:** nativo (`cdc_acm`, `cp210x`, `ch341`) + grupo `dialout`/`uucp` y reglas `udev`<br>• **Windows:** driver USB-JTAG/CDC de Espressif (o CP210x/CH34x según puente de placa)<br>• **macOS:** soporte nativo para USB-CDC y VCP | Consultar [docs/entorno_desarrollo.md](docs/entorno_desarrollo.md) para el detalle en Linux |
| **Editor / IDE** | **Agnóstico** (100 % operable desde terminal) | Compatible con terminal pura, VS Code (extensión ESP-IDF), Neovim, CLion u otro editor |
| **Herramientas opcionales (`tools/`)** | Python 3 + `pyserial` (ingesta de logs `[STR_METRIC]`) | Opcional: `matplotlib`/`pandas` o exportación CSV para gráficas del informe STR |

---

## 3. Requerimientos de Hardware (BOM y Alternativas Equivalentes)

### 3.1 Lista de Materiales (BOM de Referencia)

| Cantidad | Componente | Interfaz / Alimentación | Función en el sistema |
| :---: | :--- | :--- | :--- |
| **2** | Placa `ESP32-C6-DevKitC-1` | USB-C / 3.3V y 5V | **Nodo 1** (Acceso) y **Nodo 2** (Seguridad) |
| **1** | Placa `ESP32-C6 SuperMini` | USB-C / 3.3V | **Nodo 3** (Climatización) |
| **1** | Placa `ESP32-S3 N16R8` (formato UNO) | USB-C / 16 MB Flash + 8 MB Octal PSRAM | **Gateway / Border Router** BLE Mesh + Wi-Fi + Telegram |
| **1** | Módulo lector **RFID-RC522** + ≥ 2 tarjetas/llaveros MIFARE (13.56 MHz) | SPI / **estrictamente 3.3V** | Identificación de tarjetas autorizadas/rechazadas |
| **1** | Sensor de movimiento **PIR HC-SR501** | GPIO digital / alim. **5V**, señal **3.3V** | Detección de intrusión por interrupción |
| **1** | Sensor de temperatura/humedad **DHT11** | Bus 1-wire vía **RMT** / 3.3V (+ *pull-up* 4.7 kΩ–10 kΩ si es de 4 pines sin PCB) | Telemetría ambiental y control automático |
| **3** | LEDs estándar + **3** resistencias (220 Ω – 330 Ω) | GPIOs digitales / PWM `LEDC` | Emulación de cerradura, alarma tipo carro y ventilador DC |
| **—** | Protoboards, cables jumper Dupont y cables USB de datos | — | Interconexión y programación/alimentación |

### 3.2 Equivalencias y Sustitutos (para no depender de referencias únicas)

| Elemento original | Sustituto equivalente compatible | Ajuste mínimo necesario |
| :--- | :--- | :--- |
| `ESP32-C6-DevKitC-1` / `SuperMini` | Cualquier placa `ESP32-C6` (o `ESP32-S3`/`ESP32-C3` con BLE 5) | Verificar ≥ 6 GPIOs libres en Nodo 1 y ≥ 2 en Nodos 2/3; actualizar mapeo en `pinout` y `idf.py set-target` |
| `ESP32-S3 N16R8 UNO` | Cualquier `ESP32-S3` (DevKitC-1, etc.) con Wi-Fi + BLE 5 | Recomendable variante con PSRAM (`R2`/`R8`); si no posee PSRAM, deshabilitar `CONFIG_SPIRAM` en `sdkconfig` y ajustar tamaños de buffer TLS |
| Sensor `DHT11` | `DHT22` (`AM2302`) | Ajustar escalado de bits (16 bits con signo) en el decodificador RMT del Nodo 3 |
| LED Cerradura / LED Ventilador | Cerradura solenoide (relé/MOSFET + diodo *flyback*) / Ventilador DC 5V–12V (MOSFET + PWM) | Mismas señales lógicas GPIO / PWM desde ESP32-C6 |
| Solo LED de alarma | LED + buzzer piezoeléctrico activo/pasivo en paralelo o GPIO adicional | Añadir pin opcional en Nodo 2 |

---

## 4. Niveles de Replicación sin Depender del Hardware Completo

El repositorio permite validar el proyecto progresivamente según el equipamiento disponible:

1. **Nivel 1 — Compilación y verificación estática (0 placas físicas):**
   - Basta tener instalado **ESP-IDF `v6.1.1`** en Linux, Windows o macOS.
   - Permite compilar los 4 firmwares (`idf.py build` para `esp32c6` y `esp32s3`), inspeccionar tamaños de binario/RAM (`idf.py size`) y validar la estructura de `components/common_types` y `components/mesh_protocol`.
2. **Nivel 2 — Pruebas de lógica, FSM y Telegram sin sensores físicos (Host Linux / 1 placa):**
   - La lógica de negocio (FSM de alarma de 3 estados, tabla de UIDs, histéresis de ventilador y codificación/decodificación binaria del *Vendor Model*) está desacoplada de los drivers SPI/RMT/GPIO.
   - Puede ejecutarse con pruebas unitarias en host (Unity / Linux target de ESP-IDF) o inyectando eventos simulados por consola serial en una sola placa ESP32-S3 / ESP32-C6 sin conectar RC522, HC-SR501 ni DHT11.
3. **Nivel 3 — Validación completa de malla BLE Mesh y radiofrecuencia (2 a 4 placas reales):**
   - **Nota técnica:** los emuladores como QEMU no simulan las pilas de radio física **BLE Mesh 2.4 GHz**, el árbitro de coexistencia `esp_coex` ni el enrutamiento multi-salto (*Relay*). Por ello, las pruebas reales de radio, aprovisionamiento PB-ADV y métricas experimentales (`E1–E6`) requieren al menos **2 placas físicas** (1× `ESP32-S3` + 1× `ESP32-C6`) y **4 placas** para el escenario completo con *Relay* multi-salto.

---

## 5. Requerimientos de Red y Servicios Externos

- **Conectividad Wi-Fi:** punto de acceso **2.4 GHz** con salida a Internet por **HTTPS (puerto 443)** hacia `api.telegram.org`. Se recomienda disponer de una segunda red de respaldo (p. ej., hotspot móvil) para conmutación automática (*failover*).
- **Bot y Grupo de Telegram:**
  - Bot creado con `@BotFather` (obteniendo `TELEGRAM_BOT_TOKEN`).
  - *Group Privacy* desactivado (`/setprivacy` → `Disable`) para operar dentro del grupo autorizado.
  - Grupo de Telegram creado y su identificador numérico (`TELEGRAM_AUTHORIZED_CHAT_ID`).
- **Archivo local de credenciales:** copiar `secrets.example.h` a `firmware/gateway_s3_uno/main/secrets.h` (ignorado por `.gitignore`) antes de compilar el Gateway.

---

## 6. Checklist Rápido Previo a la Compilación

1. [ ] **ESP-IDF `v6.1.1`** activo en terminal (`idf.py --version` → `ESP-IDF v6.1.1`).
2. [ ] Compiladores **`riscv32-esp-elf-gcc`** (ESP32-C6) y **`xtensa-esp-elf-gcc`** (ESP32-S3) disponibles en el `PATH` tras activar el entorno.
3. [ ] En Linux: permisos seriales (`dialout`/`uucp`) y reglas `udev` activos para leer/flashear `/dev/ttyACM*` o `/dev/ttyUSB*` sin `sudo`.
4. [ ] Archivo `secrets.h` configurado con 1 o 2 redes Wi-Fi, token del bot y `chat_id` autorizado (cuando se compile `gateway_s3_uno`).
5. [ ] Cableado de 3.3V verificado antes de energizar (especialmente **RFID-RC522 a 3.3V** y **PIR HC-SR501 a 5V**).
