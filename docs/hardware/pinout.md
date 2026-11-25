# Especificación de Hardware y Pinout (Fase 1)

Este documento define el mapeo definitivo de pines, criterios de selección de placas, restricciones eléctricas y código de señalización visual para los **4 microcontroladores** del proyecto.

---

## 1. Asignación de placas y justificación

| Nodo | Placa asignada | Justificación técnica |
| :--- | :--- | :--- |
| **Nodo 1 — Acceso** | **ESP32-C6-DevKitC-1** | El lector RFID-RC522 requiere **5 señales SPI/control** (`SCK`, `MOSI`, `MISO`, `CS`, `RST`) más **1 GPIO** para el LED de cerradura (6 GPIOs externos). La DevKitC-1 expone todos los pines sin compromisos de strapping. |
| **Nodo 2 — Seguridad** | **ESP32-C6-DevKitC-1** | Aunque el PIR HC-SR501 + LED de alarma requieren solo 2 GPIOs de aplicación, la DevKitC-1 entrega alimentación **5V estable** en su pin dedicado para el regulador del HC-SR501 y mayor comodidad de montaje en protoboard. |
| **Nodo 3 — Climatización** | **ESP32-C6 SuperMini** | Es el nodo con **menor requerimiento de pines** (solo **1 GPIO** externo de señal: `DATA` del DHT11 a **3.3V**). Se adapta perfectamente al formato compacto de la SuperMini. |
| **Gateway / Maestro BLE** | **ESP32-S3 N16R8 (UNO)** | Cuenta con **16 MB Flash (`N16`)**, **8 MB Octal PSRAM (`R8`)**, doble núcleo a 240 MHz y radios BLE 5 + Wi-Fi para alojar el Maestro BLE GATT (estrella) + coexistencia Wi-Fi + mbedTLS/HTTPS de Telegram. |

> **Reglas globales de diseño físico:**
> - **Sin buzzer:** la alarma tipo carro en Nodo 2 se señaliza exclusivamente por patrones temporales en su LED externo (y en el grupo de Telegram).
> - **Sin pines GPIO de traza:** toda la instrumentación de Sistemas en Tiempo Real (latencias, *jitter*, *deadline misses*) se realiza **100 % por software** mediante `esp_timer_get_time()`.

---

## 2. Pines reservados y restricciones en ESP32-C6

Antes de asignar periféricos en las 3 placas ESP32-C6, se excluyen los pines con funciones críticas internas o de arranque:

| Pines ESP32-C6 | Función reservada / Advertencia | Regla en el proyecto |
| :--- | :--- | :--- |
| `GPIO 24 – GPIO 30` | Bus SPI de la Flash interna del encapsulado | **Prohibido usar** |
| `GPIO 12`, `GPIO 13` | USB Serial / JTAG nativo (D− / D+) | **Prohibido usar** (reservados para consola/flasheo USB) |
| `GPIO 16`, `GPIO 17` | UART0 (`TX0`, `RX0`) | **Reservados** para depuración UART estándar |
| `GPIO 4`, `GPIO 5`, `GPIO 15` | Pines de *Strapping* (junto a `GPIO 8` y `GPIO 9`) | Evitar cargas que fijen estado lógico durante el reset |
| `GPIO 8` | LED RGB direccionable integrado (`WS2812`) + *Strapping* | Uso exclusivo para **LED RGB de estado BLE Mesh** |
| `GPIO 9` | Botón `BOOT` integrado + *Strapping* (pull-up interno) | Uso exclusivo para **botón Factory Reset (5 s)** |

---

## 3. Pinout detallado por placa

### 3.1 Nodo 1 — Control de Acceso (`ESP32-C6-DevKitC-1`)

**Periféricos:** Lector **RFID-RC522** (bus SPI2) + **LED Cerradura** (enclavado / toggle).

| Señal / Componente | GPIO ESP32-C6 | Dirección / Periférico | Notas eléctricas y de firmware |
| :--- | :--- | :--- | :--- |
| RC522 `MISO` | **`GPIO 2`** | Entrada (`SPI2_HOST`) | Señal 3.3V |
| RC522 `SCK` | **`GPIO 6`** | Salida (`SPI2_HOST`) | Señal 3.3V, reloj SPI ≤ 4–10 MHz |
| RC522 `MOSI` | **`GPIO 7`** | Salida (`SPI2_HOST`) | Señal 3.3V |
| RC522 `SDA (CS)` | **`GPIO 18`** | Salida (`SPI2_CS0`) | Chip Select activo en bajo |
| RC522 `RST` | **`GPIO 19`** | Salida Digital | Reset hardware del módulo MFRC522 |
| **LED Cerradura** | **`GPIO 20`** | Salida Digital | Ánodo al GPIO → resistencia **220 Ω – 330 Ω** → LED → `GND`. `HIGH` = Cerradura ABIERTA, `LOW` = Cerradura CERRADA |
| LED RGB integrado | **`GPIO 8`** | Salida (`RMT` / `led_strip`) | Indicador de estado BLE Mesh |
| Botón `BOOT` | **`GPIO 9`** | Entrada (Pull-up) | Mantener presionado ≥ 5 s → borra NVS BLE Mesh + reinicia |
| Alimentación RC522 | **`3V3` / `GND`** | Alimentación | **ADVERTENCIA:** el RC522 opera **estrictamente a 3.3V**. No conectar a `5V` bajo ninguna circunstancia (daño permanente al chip MFRC522). |

---

### 3.2 Nodo 2 — Seguridad e Intrusión (`ESP32-C6-DevKitC-1`)

**Periféricos:** Sensor de movimiento **PIR HC-SR501** + **LED Alarma tipo carro**.

| Señal / Componente | GPIO ESP32-C6 | Dirección / Periférico | Notas eléctricas y de firmware |
| :--- | :--- | :--- | :--- |
| **PIR HC-SR501 `OUT`** | **`GPIO 3`** | Entrada Digital (`ISR`) | Configurado con `GPIO_INTR_POSEDGE` y *pull-down* interno. La salida lógica del HC-SR501 es **3.3V** (segura para ESP32-C6). |
| **LED Alarma** | **`GPIO 20`** | Salida Digital (`xTimer`) | Ánodo al GPIO → resistencia **220 Ω – 330 Ω** → LED → `GND`. Ejecuta patrones de alarma automotriz (`DESARMADA`, `ARMADA`, `DISPARADA`). |
| LED RGB integrado | **`GPIO 8`** | Salida (`RMT` / `led_strip`) | Indicador de estado BLE Mesh |
| Botón `BOOT` | **`GPIO 9`** | Entrada (Pull-up) | Mantener presionado ≥ 5 s → borra NVS BLE Mesh + reinicia |
| Alimentación PIR | **`5V` / `GND`** | Alimentación | El módulo HC-SR501 usa un regulador lineal interno HT7133 que requiere **5V en `VCC`** para trabajar sin falsos disparos. |

#### Calibración obligatoria del HC-SR501 antes de pruebas STR
1. **Potenciómetro de tiempo (`Tx`):** girar completamente al mínimo (sentido antihorario) para obtener pulsos cortos (~2–3 s) y evitar que la salida quede retenida minutos.
2. **Potenciómetro de sensibilidad (`Sx`):** ajustar a un rango medio-bajo en pruebas de escritorio para evitar falsos positivos por movimiento lateral lejano.
3. **Jumper de disparo:** ubicar en posición **`H` (*Repeatable Trigger*)** para re-disparos continuos mientras haya presencia, o **`L` (*Single Trigger*)** si se quiere pulso único por intrusión (recomendado **`H`** para seguridad continua).
4. **Tiempo de estabilización al encendido:** el sensor PIR requiere **30–60 s** de estabilización térmica tras energizar; el firmware aplicará un *masking* inicial o indicación de calibración al arrancar para no disparar falsas alarmas durante el *boot*.

---

### 3.3 Nodo 3 — Climatización (`ESP32-C6 SuperMini`)

**Periféricos:** Sensor **DHT11** (temperatura/humedad para alertas por sobresalto térmico).

| Señal / Componente | GPIO ESP32-C6 | Dirección / Periférico | Notas eléctricas y de firmware |
| :--- | :--- | :--- | :--- |
| **DHT11 `DATA`** | **`GPIO 2`** | Bidireccional (`RMT` TX/RX) | Lectura no bloqueante usando periférico hardware **RMT** cada **3 s**. Requiere resistencia *pull-up* de **4.7 kΩ – 10 kΩ** a `3.3V` (si el módulo PCB de 3 pines ya la trae integrada, no añadir otra). |
| LED RGB integrado | **`GPIO 8`** | Salida (`RMT` / `led_strip`) | Indicador de estado BLE GATT y alerta térmica activa (rojo intenso). |
| Botón `BOOT` | **`GPIO 9`** | Entrada (Pull-up) | Mantener presionado ≥ 5 s → borra NVS + reinicia. |
| Alimentación DHT11 | **`3V3` / `GND`** | Alimentación | Alimentar a **3.3V** para mantener niveles lógicos compatibles sin divisor resistivo. |

---

### 3.4 Gateway / Maestro BLE GATT (`ESP32-S3 N16R8` formato UNO)

La placa **ESP32-S3 N16R8 en formato Arduino UNO** no conecta sensores de campo; actúa exclusivamente como **Maestro/Central BLE GATT (topología estrella)**, **Coordinador de estado**, **Gateway Wi-Fi** y **Cliente HTTPS del Bot de Telegram**.

| Recurso / Pin | GPIO ESP32-S3 | Función | Notas de configuración |
| :--- | :--- | :--- | :--- |
| Botón `BOOT` | **`GPIO 0`** | Entrada (Pull-up) | Mantener presionado ≥ 5 s → limpia NVS BLE Mesh (y fuerza re-aprovisionamiento de la red si se desea). |
| LED RGB integrado | **`GPIO 48`** *(o `GPIO 38` según rev.)* | Salida (`RMT`) | Confirmar en el esquemático/serigrafía de la placa UNO exacta y dejar configurable vía `Kconfig` (`menuconfig`). |
| Bus Octal PSRAM (`R8`) | `GPIO 33 – GPIO 37` | Interno (Octal SPI) | **PROHIBIDO USAR:** conectados internamente a los 8 MB de Octal PSRAM. |
| Bus Flash SPI (`N16`) | `GPIO 26 – GPIO 32` | Interno (Quad/Octal SPI) | **PROHIBIDO USAR:** conectados internamente a los 16 MB de Flash. |
| USB Nativo (CDC/JTAG) | `GPIO 19`, `GPIO 20` | USB `D−` / `D+` | **PROHIBIDO USAR** si la consola o depuración se conecta por puerto USB nativo. |
| Pines de *Strapping* | `GPIO 0`, `GPIO 3`, `GPIO 45`, `GPIO 46` | Arranque / Voltaje Flash | Precaución en caso de añadir periféricos auxiliares en el futuro. |

> **Advertencia sobre formato UNO:** En placas ESP32-S3 con factor de forma Arduino UNO, las etiquetas impresas (`D0..D13`, `A0..A5`) suelen remapease a distintos GPIOs del S3 según el fabricante (por ejemplo clones tipo *ESP32-S3-UNO* / *Waveshare* / genéricos). **Siempre referirse al GPIO real del SoC ESP32-S3** en el código ESP-IDF y verificar el mapa de pines impreso en la tarjeta antes de usar cualquier cabecera lateral.

#### Lineamientos ESP-IDF para memoria en `gateway_s3_uno`
- Activar Flash de 16 MB y Octal PSRAM de 8 MB:
  - `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`
  - `CONFIG_SPIRAM=y`
  - `CONFIG_SPIRAM_MODE_OCT=y`
- Política de ubicación en memoria:
  - **Octal PSRAM:** buffers de sesión **mbedTLS**, búferes HTTP de Telegram (`esp_http_client`), parseo JSON (`cJSON`) y colas no críticas.
  - **SRAM interna:** stacks de todas las tareas críticas FreeRTOS, rutinas ISR, estructuras del controlador BLE/Wi-Fi y colas de mensajería rápida.

---

## 4. Código de señalización unificado (LED RGB integrado)

Todas las placas comparten un esquema visual coherente en su LED RGB integrado para diagnóstico rápido en campo y durante la sustentación:

| Color / Patrón RGB | Significado en Nodos ESP32-C6 (1, 2 y 3) | Significado en Maestro ESP32-S3 |
| :--- | :--- | :--- |
| **Rojo fijo (`DISCONNECTED`)** | Esperando conexión del Maestro S3 (Advertising BLE activo) | Error crítico o sin ningún periférico conectado |
| **Azul parpadeante (`CONNECTING`)** | Emparejamiento / descubrimiento GATT en curso | Escaneando / conectando periféricos C6 o conectando Wi-Fi |
| **Ámbar / Amarillo (`WIFI_OFFLINE`)** | *(No aplica)* | Estrella BLE operativa, pero **Wi-Fi o Telegram desconectado** (modo offline) |
| **Verde fijo (`CONNECTED`)** | Conectado al Maestro S3 por BLE GATT (`0xFF10`) | **Sistema 100 % operativo:** 3× C6 conectados + Wi-Fi + Telegram activos |
| **Cian destello breve (`ACTIVITY`)** | Transmisión / recepción de evento o comando GATT | Procesando comando desde Telegram o reenviando evento N1 ↔ N2 |
| **Rojo intermitente rápido (`ALARM_TRIGGERED`)** | En Nodo 2: alarma `DISPARADA` \| En Nodo 3: alerta térmica activa | Alarma global en estado `DISPARADA` o alerta térmica crítica |
| **Morado fijo (durante 5 s)** | Botón `BOOT` mantenido: preparando *Factory Reset* de NVS | Botón `BOOT` mantenido: preparando *Factory Reset* de NVS |
