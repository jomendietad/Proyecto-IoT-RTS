# Entorno de Desarrollo y Verificación en Linux (ESP-IDF v6.1.1)

Este documento describe el entorno de desarrollo de referencia sobre **Fedora Linux 45** con **ESP-IDF `v6.1.1`**, la configuración de un entorno Python aislado, los pasos agnósticos para verificar permisos seriales/`udev` y el flujo de trabajo completo por terminal.

---

## 1. Entorno de Desarrollo de Referencia

El proyecto se desarrolla bajo un flujo **100 % por terminal** (sin dependencia obligatoria de IDE) sobre Linux x86_64.

| Componente | Versión / Detalle verificado | Notas |
| :--- | :--- | :--- |
| **Sistema Operativo** | Fedora Linux 45 (x86_64, kernel `7.x`) | Reproducible en cualquier distribución Linux reciente |
| **SDK** | **ESP-IDF `v6.1.1`** | Instalado vía **EIM** en `$HOME/.espressif/v6.1.1/esp-idf` |
| **Targets instalados** | `esp32c6`, `esp32s3` | Cubre los 3 nodos de campo y el Gateway |
| **Python del entorno (`venv`)** | **`3.13.18`** (standalone aislado) | Ubicado en `$HOME/.espressif/python313` y `$HOME/.espressif/tools/python/v6.1.1/venv` |
| **Compilador ESP32-C6** | `riscv32-esp-elf-gcc` **`15.2.0`** | Paquete `esp-15.2.0_20251204` |
| **Compilador ESP32-S3** | `xtensa-esp-elf-gcc` **`15.2.0`** | Paquete `esp-15.2.0_20251204` |
| **Build System** | `CMake 4.0.3` + `Ninja 1.12.1` | Gestionados junto a los toolchains en `$HOME/.espressif/tools` |
| **Flasheo y Depuración** | `esptool v5.6.0` + `openocd-esp32` | Incluidos en el entorno EIM |
| **Utilidades base del sistema** | `git`, `ccache`, `dfu-util`, `picocom` / `minicom` | Instaladas desde los repositorios del SO |

---

## 2. Instalación Reproducible en Fedora 45 (Python Aislado)

En distribuciones recientes como Fedora 45, `/usr/bin/python3` apunta a **Python 3.15+**, mientras que el instalador **EIM (Espressif Installation Manager)** exige **Python 3.10 – 3.13**. Además, enlazar el `venv` de ESP-IDF al Python del sistema provoca roturas cuando la distribución actualiza su versión menor de Python.

### Solución recomendada en espacio de usuario (sin `sudo`)

1. **Instalar un intérprete Python 3.13 standalone en `$HOME/.espressif/python313`:**
   - Descargar una compilación standalone oficial (por ejemplo desde `astral-sh/python-build-standalone`, archivo `cpython-3.13.x-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz`) y extraerlo en `$HOME/.espressif/python313`:
     ```bash
     mkdir -p "$HOME/.espressif/python313"
     tar -xzf cpython-3.13.*.tar.gz --strip-components=1 -C "$HOME/.espressif/python313"
     "$HOME/.espressif/python313/bin/python3" --version
     ```
2. **Ejecutar la instalación de ESP-IDF `v6.1.1` con EIM usando ese Python:**
   ```bash
   PATH="$HOME/.espressif/python313/bin:$PATH" \
     "$HOME/.espressif/eim_gui/eim" install \
       -i v6.1.1 \
       -t esp32c6,esp32s3 \
       -p "$HOME/.espressif" \
       -n true \
       --cleanup true
   ```
   De este modo, el `venv` creado en `$HOME/.espressif/tools/python/v6.1.1/venv` queda permanentemente ligado a Python 3.13 y protegido frente a futuras actualizaciones de `/usr/bin/python3`.

> **Alternativa en Fedora (repositorios oficiales):** si se prefiere usar paquetes del sistema, puede instalarse `python3.13` con `sudo dnf install python3.13` y anteponer esa ruta o crear el entorno apuntando a `python3.13`.

---

## 3. Verificación Agnóstica de Permisos en Linux (Serial + `udev`)

Para flashear y monitorear placas **ESP32-C6** y **ESP32-S3** sin ejecutar comandos con `sudo`, deben cumplirse dos condiciones en cualquier sistema Linux: acceso al grupo serial y reglas `udev` para USB-JTAG/USB-Serial.

### 3.1 Permiso de puertos seriales (`/dev/ttyACM*`, `/dev/ttyUSB*`)

- **Verificación genérica:**
  ```bash
  id -nG "$USER" | tr ' ' '\n' | grep -qx dialout \
    && echo "OK: permiso dialout activo" \
    || echo "Falta agregar el usuario al grupo dialout"
  ```
- **Cómo otorgarlo (si hace falta):**
  - En **Fedora / Debian / Ubuntu**:
    ```bash
    sudo usermod -aG dialout "$USER"
    ```
    *(Es obligatorio cerrar sesión y volver a iniciar para que el cambio de grupo tome efecto).*
  - En **Arch Linux / derivadas**, el grupo equivalente para puertos seriales es `uucp`.

### 3.2 Reglas `udev` para USB-JTAG integrado (Espressif) y `ModemManager`

Tanto el **ESP32-C6** como el **ESP32-S3** integran controlador USB Serial/JTAG nativo con **Vendor ID `303a`** (`Product ID 1001` y `1002`), además de posibles puentes USB-UART externos (CP210x `10c4:ea60`, CH340/CH343 `1a86:*`).

- **Verificación genérica de reglas existentes:**
  ```bash
  grep -rn "303a" /etc/udev/rules.d/ /usr/lib/udev/rules.d/ 2>/dev/null
  ```
- **Regla de referencia (si no existe en el equipo):** crear `/etc/udev/rules.d/99-esp32.rules` con:
  ```udev
  # Espressif USB Serial/JTAG y CDC (ESP32-C6 / ESP32-S3)
  SUBSYSTEMS=="usb", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", MODE="0660", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{ID_MM_PORT_IGNORE}="1"
  SUBSYSTEMS=="usb", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1002", MODE="0660", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{ID_MM_PORT_IGNORE}="1"
  ```
  Y recargar el subsistema:
  ```bash
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```
  *(Nota: `ENV{ID_MM_DEVICE_IGNORE}="1"` evita que `ModemManager` capture brevemente `/dev/ttyACM*` al conectar la placa e interfiera con `esptool` o el monitor serie).*

### 3.3 Comprobación rápida al conectar una placa
```bash
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

---

## 4. Flujo de Trabajo Diario por Terminal

### Activación de sesión
En cada terminal donde se vaya a compilar o flashear:
```bash
source "$HOME/.espressif/tools/activate_idf_v6.1.1.sh"
```
*(Opcional)* Agregar un alias en `~/.bashrc` para activarlo bajo demanda:
```bash
alias get_idf='source "$HOME/.espressif/tools/activate_idf_v6.1.1.sh"'
```

Para ejecutar comandos puntuales sin modificar la sesión actual:
```bash
"$HOME/.espressif/eim_gui/eim" run "idf.py --version" v6.1.1
```

### Ciclo estándar por firmware
Dentro de cada proyecto en `firmware/<nombre_proyecto>/`:
1. **Seleccionar chip objetivo** (solo la primera vez o tras `fullclean`):
   - Nodos 1, 2 y 3: `idf.py set-target esp32c6`
   - Gateway S3: `idf.py set-target esp32s3`
2. **Configurar opciones** (PSRAM, NimBLE GATT, coexistencia Wi-Fi + BLE, etc.):
   ```bash
   idf.py menuconfig
   ```
3. **Compilar:**
   ```bash
   idf.py build
   ```
4. **Flashear y abrir monitor serial:**
   ```bash
   idf.py -p /dev/ttyACM0 flash monitor
   ```
   *(Ajustar `/dev/ttyACM0` o `/dev/ttyUSB0` según el puerto detectado; salir del monitor con `Ctrl + ]`).*

### Compatibilidad opcional con IDE
El instalador EIM registra la instalación activa en `$HOME/.espressif/tools/eim_idf.json`. Si posteriormente se utiliza **VS Code** con la extensión oficial de ESP-IDF (u otro editor), esta detectará automáticamente la versión `v6.1.1` y sus toolchains sin configuraciones adicionales.
