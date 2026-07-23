# public_ESP32OTA

Firmware de ejemplo para ESP32 que hace:

1. Provisión de credenciales WiFi por puerto Serial (guardadas en NVS).
2. Consulta de la última versión publicada en los **GitHub Releases** de este repo.
3. Actualización OTA (descarga + flasheo) si hay una versión más nueva que la guardada localmente.
4. Blink de un LED en el loop principal + comandos por Serial (`ota`, `wifi`).

## Visión general: dos implementaciones, un solo repo

El mismo comportamiento está implementado **dos veces**, en dos frameworks distintos, para poder comparar/migrar entre ellos:

| Carpeta | Framework | Tag de sus releases |
|---|---|---|
| [`blink_OTA/`](blink_OTA/) | Arduino core para ESP32 | `arduino-v1`, `arduino-v2`, `arduino-v3`... |
| [`esp32_ota_idf/`](esp32_ota_idf/) | ESP-IDF v6.0.2 nativo | `idf-v1`, `idf-v2`, `idf-v3`... |

Cada proyecto tiene su propio pipeline de CI/CD (dispara solo con cambios en su carpeta) y publica sus propios releases **en este mismo repo GitHub**, usando `GET /repos/codergear/public_ESP32OTA/releases` con `per_page=10`.

Como ambos publican al mismo repo, cada firmware **filtra los releases por el prefijo de su propio tag e ignora los del otro**:

- El firmware **Arduino** recorre la lista de releases y se queda con el primero cuyo tag empieza con `arduino-v` (ej: `arduino-v3`). Si ve `idf-v5` en la lista, lo salta.
- El firmware **ESP-IDF** hace lo mismo pero al revés: busca el primer tag que empiece con `idf-v` (ej: `idf-v3`) y salta cualquier `arduino-vN`.

Esto evita que, por ejemplo, un dispositivo con firmware Arduino intente aplicar por OTA un `.bin` compilado con ESP-IDF (no son binarios compatibles entre sí).

> Si forkeás este repo, cambiá `GITHUB_API_URL` en `blink_OTA/blink_OTA.ino` **y** en `esp32_ota_idf/main/app_main.c` para que apunten a tu propio `usuario/repo`.

### ⚠️ Nota de seguridad (aplica a ambos)

Ninguna de las dos implementaciones valida el certificado TLS del servidor (`WiFiClientSecure::setInsecure()` en Arduino, `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` en ESP-IDF). Es intencional, para mantener el comportamiento simple del proyecto original — no usar así en producción sin agregar validación de certificados.

---

## Proyecto Arduino (`blink_OTA/`)

- Tag de sus releases: **`arduino-v<N>`** (ej. `arduino-v1`). Firmware busca ese prefijo específico en `GET /releases` y descarta cualquier `idf-vN`.
- Versión local guardada en NVS, namespace `"version"`, key `"local"` (namespace `"wifi"` para SSID/pass).

### Prerrequisitos

- [Arduino IDE](https://www.arduino.cc/en/software) o [`arduino-cli`](https://arduino.github.io/arduino-cli/latest/installation/)
- Board package **esp32 by Espressif Systems** (vía Boards Manager, o `arduino-cli core install esp32:esp32`)
- Librería **ArduinoJson** (vía Library Manager, o `arduino-cli lib install "ArduinoJson"`)
- Placa: ESP32 Dev Module (WROOM), flash de 4MB
- Cable USB + drivers del chip USB-serial de tu placa (CP2102/CH340/etc.)

### Compilar y flashear

Con Arduino IDE: abrir `blink_OTA/blink_OTA.ino`, seleccionar board **ESP32 Dev Module**, puerto correspondiente, y **Upload**.

Con `arduino-cli` (mismo procedimiento que usa el CI):

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "ArduinoJson"

arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir build ./blink_OTA
arduino-cli upload -p <PUERTO> --fqbn esp32:esp32:esp32 ./blink_OTA
```

Monitor serial: 115200 baud (`arduino-cli monitor -p <PUERTO> -c baudrate=115200`, o el Serial Monitor del IDE).

### Comandos por Serial

| Comando | Efecto |
|---|---|
| `ota` | Fuerza el chequeo de versión (busca `arduino-vN`) + actualización OTA |
| `wifi` | Vuelve a pedir SSID/contraseña y reinicia |

Si no hay credenciales guardadas en NVS (primer arranque), el firmware entra solo en modo configuración y pide `SSID:` / `Password:` por Serial.

### CI/CD

`.github/workflows/build-arduino.yml`:

- Se dispara con cambios en `blink_OTA/**` (push a `main`, o manual con `workflow_dispatch`).
- Compila con `arduino-cli` instalado en el runner (`ubuntu-latest`, sin contenedor especial).
- Publica el `.bin` como asset de un GitHub Release con tag `arduino-v${{ github.run_number }}`.

---

## Proyecto ESP-IDF (`esp32_ota_idf/`)

- Tag de sus releases: **`idf-v<N>`** (ej. `idf-v1`). Firmware busca ese prefijo específico en `GET /releases` y descarta cualquier `arduino-vN`.
- Versión local guardada en NVS, namespace `"version"`, key `"local"` (namespace `"wifi"` para SSID/pass) — mismo esquema que el proyecto Arduino.

### Prerrequisitos

- [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/get-started/index.html) instalado (toolchain + `idf.py`). Instalador oficial para Windows/Linux/macOS.
- Target: `esp32`
- Placa: ESP32 Dev Module (WROOM), flash de 4MB (la tabla de particiones en `partitions.csv` asume 4MB — ajustar si tu placa tiene más)
- Conexión a internet la primera vez que se compila: el **IDF Component Manager** descarga automáticamente la dependencia `cjson` declarada en `main/idf_component.yml`

### Compilar y flashear

```bash
cd esp32_ota_idf

# Cargar el entorno de IDF (una vez por terminal)
. $IDF_PATH/export.sh        # Linux/macOS
# %IDF_PATH%\export.bat      # Windows cmd
# . $IDF_PATH\export.ps1     # Windows PowerShell

idf.py set-target esp32
idf.py build

idf.py -p <PUERTO> flash monitor
```

El **primer flasheo tiene que ser por cable** (`idf.py flash`), ya que graba también el bootloader y la tabla de particiones. Las actualizaciones siguientes ya llegan solas por OTA.

### Comandos por Serial

| Comando | Efecto |
|---|---|
| `ota` | Fuerza el chequeo de versión (busca `idf-vN`) + actualización OTA |
| `wifi` | Vuelve a pedir SSID/contraseña y reinicia |

Si no hay credenciales guardadas en NVS (primer arranque), el firmware entra solo en modo configuración y pide `SSID:` / `Password:` por Serial (115200 baud).

### Estructura del proyecto

```
esp32_ota_idf/
├── CMakeLists.txt          # top-level, project(esp32_ota_idf)
├── sdkconfig.defaults      # TLS inseguro (ver nota de seguridad), tabla de particiones custom
├── partitions.csv          # esquema OTA: nvs, otadata, phy_init, ota_0, ota_1 (4MB flash)
└── main/
    ├── CMakeLists.txt      # componentes requeridos (REQUIRES)
    ├── idf_component.yml   # dependencia gestionada: espressif/cjson
    └── app_main.c           # puerto 1:1 de blink_OTA.ino
```

### Notas del port a ESP-IDF v6.0.2

Por si tocás el código y te chocás con errores de compilación parecidos:

- Desde **ESP-IDF v6.0**, `cJSON` dejó de venir incluido en el core y se agrega vía Component Registry (`main/idf_component.yml`).
- El componente legado `driver` se dividió en componentes granulares (`esp_driver_gpio`, `esp_driver_uart`, etc.) — ver `REQUIRES` en `main/CMakeLists.txt`.
- `esp_https_ota.h` incluye `bootloader_common.h`, que solo es alcanzable vía una dependencia privada (`app_update` → `bootloader_support`) — por eso `bootloader_support` está declarado explícito en `REQUIRES`.
- La vieja API `esp_vfs_dev_uart_use_driver()` fue eliminada en v6.0; el reemplazo es `uart_vfs_dev_use_driver()` (`driver/uart_vfs.h`).
- `nvs_get_i32`/`nvs_set_i32` requieren `int32_t*` exacto (no `int*`) en algunos toolchains — la variable `versionLocal` está tipada como `int32_t` por eso.

### CI/CD

`.github/workflows/build-idf.yml`:

- Se dispara con cambios en `esp32_ota_idf/**` (push a `main`, o manual con `workflow_dispatch`).
- Compila dentro del contenedor Docker oficial `espressif/idf:v6.0.2` (mismo toolchain que documentás arriba, pero reproducible sin instalar nada en el runner).
- Publica el `.bin` como asset de un GitHub Release con tag `idf-v${{ github.run_number }}`.

---

## Replicar este proyecto desde cero (resumen)

1. Forkear/clonar este repo.
2. Cambiar `GITHUB_API_URL` en `blink_OTA/blink_OTA.ino` y `esp32_ota_idf/main/app_main.c` para que apunte a tu `usuario/repo`.
3. Elegir un proyecto (Arduino o ESP-IDF) y seguir su sección correspondiente más arriba para compilar y flashear por cable la primera vez.
4. Al conectar por Serial (115200 baud), cargar el SSID/contraseña de tu WiFi cuando se pidan.
5. Hacer push a `main` tocando la carpeta del proyecto elegido → el workflow correspondiente compila y publica un release (`arduino-v1` o `idf-v1`).
6. Reiniciar el dispositivo o mandar el comando `ota` por Serial: debería detectar el release recién publicado y actualizarse solo.
