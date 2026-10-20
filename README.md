# Driver IMX296 para la Orange Pi Zero 3W (Allwinner A733)

Driver VIN (el framework de cámaras de Allwinner) para la **Raspberry Pi Global Shutter Camera** (sensor Sony IMX296) en la **Orange Pi Zero 3W** (SoC Allwinner A733, familia `sun60iw2`), sobre la imagen oficial de Orange Pi con kernel 6.6. Es parte del Proyecto Ticanor: el par estéreo del subsistema de evasión de obstáculos de un dron.

La imagen de Orange Pi solo soporta la cámara IMX219. El driver de la IMX296 del kernel principal identifica el sensor, pero VIN no trabaja con subdispositivos V4L2 estándar, así que no aparece ningún `/dev/video*`. Este repositorio trae un driver VIN propio, el overlay del device tree y las herramientas para compilarlo y probarlo.

## Estado

| Qué | Estado |
|---|---|
| Detección por I2C (conectores CAM1 y CAM2) | Funciona: `SENSOR_INFO` = 0xca00, IMX296LL (mono) |
| Captura en CAM2 | RAW10 en tres modos: **1456×1088 a 60 fps**, 1456×544 (recorte) y 728×544 (binning 2×2) **a 100 fps**. Medido: fps ±0,001 % del valor calculado, sin saltos de secuencia |
| Controles en vivo (`/dev/v4l-subdevN`) | Exposición y ganancia (en el mismo cuadro), VBLANK (fps menores, exposiciones de hasta 33 ms a 30 fps o más largas), espejo, patrón de prueba y temperatura del sensor |
| Overlay para las dos cámaras | Funciona; con CAM1 vacío, VIN lo descarta sin errores |
| Dos cámaras a la vez | Sin probar |
| Disparo externo (XTR) para sincronizar el par estéreo | `trigger_mode = 1` implementado, sin probar (requiere modificar la cámara) |
| Exposición automática | Pendiente |
| Parche de VIN (`driver/vin/`) | Los dos usos de VIN que daban *kernel panic* devuelven un error (`ENODEV` / `EINVAL`); la captura normal no cambia |
| Robustez de la placa (`config/`) | Tras un *kernel panic* vuelve sola por SSH en ~46 s; si queda colgada, el watchdog la reinicia a los 16 s. El journal sobrevive a los cortes |

## Hardware probado

- Orange Pi Zero 3W de 12 GB con la imagen `Orangepizero3w_1.0.2_ubuntu_jammy_desktop_xfce_linux6.6.98` (kernel `6.6.98-sun60iw2`).
- Raspberry Pi Global Shutter Camera, versión mono (IMX296LL). Trae su propio oscilador de 54 MHz y usa 1 carril MIPI.
- Cable de 22 a 15 pines. Conviene revisarlo con multímetro: el primero que usamos tenía cortada la pista de SDA y la cámara no respondía por I2C.
- **Disipador o ventilador obligatorio.** Sin enfriamiento, la A733 se reinicia en bucle, y durante el arranque nada controla todavía el ventilador de la placa.

## Estructura

```
driver/imx296_mipi.c            driver VIN, dos instancias: imx296_mipi (CAM1) e imx296_mipi_2 (CAM2)
driver/integracion_vin.patch    Kconfig/Makefile para compilarlo dentro del kernel de Orange Pi
driver/vin/                     parches de vin_v4l2.ko para los dos panics de VIN, con su explicación
overlays/imx296-vin.dts         overlay de usuario: CAM1 y CAM2 con este driver (recomendado)
overlays/imx296-vin-cam2.dts    variante solo para CAM2
overlays/pruebas/               overlay para probar el driver del kernel principal (identifica el sensor, sin imagen)
build/                          compilación cruzada en Docker: compilar.ps1 (Windows), compilar_linux.sh (Linux),
                                compilar_vin.sh (VIN con los parches de driver/vin/)
scripts/descargar_fuentes.sh    descarga las fuentes necesarias para compilar (en el PC)
scripts/placa/                  en la placa: captura.sh, prueba_modos.sh, analizar_raw.py, raw16_a_gris8.py,
                                prueba_i2c.sh, info_placa.sh, estado_robustez.sh, prueba_termica.sh
scripts/pc/                     en el PC: raw_a_png.py, gris8_a_mp4.sh; en Windows, serie_grabar.ps1 y
                                esperar_ssh.ps1 (grabar la consola serie y medir reinicios)
config/                         configuración de la placa para que se recupere sola de un fallo (ver
                                "Robustez de la placa"), con las mismas rutas que en el sistema
```

## Compilación (en el PC, con Docker)

El módulo se compila en un contenedor x86-64 con el mismo paquete de headers que trae la imagen y el mismo compilador que usó Orange Pi para el kernel (Arm GNU Toolchain 11.2-2022.02). Así el `vermagic` coincide con el de la placa: `6.6.98-sun60iw2 SMP preempt mod_unload aarch64`. El kernel de la imagen no exige firma ni `MODVERSIONS`.

1. Descargar las fuentes (en Windows, desde Git Bash). Crea `fuentes/`, que no se versiona:
   ```bash
   bash scripts/descargar_fuentes.sh
   ```
   Entre otras cosas, exporta `bsp/drivers/vin` y `bsp/include` del commit `8a9be72` de la rama `orange-pi-6.6-sun60iw2` a `fuentes/vin-8a9be72/`. La imagen no registra con qué commit se compiló su kernel; `8a9be72` es el último anterior a esa compilación, y los `.h` de VIN cambiaron después.
2. Copiar a `build/` el paquete de headers que trae la imagen:
   ```bash
   scp orangepi@<ip-de-la-placa>:/opt/linux-headers-current-sun60iw2_1.0.0_arm64.deb build/
   ```
3. Compilar. En Windows (PowerShell):
   ```powershell
   .\build\compilar.ps1 imx296_mipi
   ```
   En Linux:
   ```bash
   build/compilar_linux.sh imx296_mipi
   ```
   El módulo queda en `build/salida/imx296_mipi.ko`. La primera vez se construye la imagen Docker `opi-a733-kmod:6.6.98` (`-Rebuild` o `--rebuild` para rehacerla). Para validar el entorno se puede compilar `imx219` y compararlo con el `imx219.ko` de la imagen.

Revisión de estilo con el `checkpatch.pl` del kernel: `build/compilar_linux.sh --checkpatch`, o en Windows:

```powershell
docker run --rm -v "${PWD}\driver:/driver:ro" opi-a733-kmod:6.6.98 sh -c 'perl $KDIR/scripts/checkpatch.pl --no-tree -f /driver/imx296_mipi.c'
```

Para compilarlo dentro del kernel completo de Orange Pi, en vez de fuera del árbol, ver las instrucciones al principio de `driver/integracion_vin.patch`.

VIN con los parches de `driver/vin/` (Linux, o Windows desde Git Bash):

```bash
build/compilar_vin.sh                       # build/salida/vin/parche/vin_v4l2.ko
build/compilar_vin.sh --original --comparar <carpeta con vin_io.ko y vin_v4l2.ko de la imagen>
```

La segunda forma valida el entorno: sin parches, el VIN compilado sale idéntico al de la imagen (`srcversion`, bytes de código y relocalizaciones). El script pasa las fuentes a LF; un checkout en Windows las deja con CRLF y entonces no coincide `srcversion` ni se aplican los parches.

## Instalación en la placa

1. Copiar a una misma carpeta de la placa (por ejemplo `~/port/`) `build/salida/imx296_mipi.ko`, `overlays/imx296-vin.dts` y el contenido de `scripts/placa/`. `captura.sh` busca el módulo en su propia carpeta; otra ubicación se indica con `PORT_DIR`.
2. Compilar el overlay e instalarlo como overlay de usuario, con respaldo previo de `orangepiEnv.txt`:
   ```bash
   dtc -@ -I dts -O dtb -o imx296-vin.dtbo imx296-vin.dts
   sudo cp -p /boot/orangepiEnv.txt /boot/orangepiEnv.txt.orig
   sudo mkdir -p /boot/overlay-user && sudo cp imx296-vin.dtbo /boot/overlay-user/
   echo "user_overlays=imx296-vin" | sudo tee -a /boot/orangepiEnv.txt
   sudo reboot
   ```
   Si `orangepiEnv.txt` ya tiene una línea `user_overlays=`, hay que agregar `imx296-vin` a esa línea en vez de crear otra. Al arrancar, U-Boot muestra `Applying user provided DT overlay imx296-vin.dtbo`.

El overlay solo cambia el nombre del driver (`sensor0_mname`, `sensor2_mname`) y la dirección I2C (0x34 en 8 bits, es decir 0x1a); el resto de los nodos VIN de la placa queda igual. Además, deja de cargar el driver `imx219`, que enciende un reloj de 24 MHz en el pin 18 del conector. En la cámara GS ese pin puede llegar a la entrada de disparo XTR.

## Uso

Los módulos se cargan en este orden, porque `vin_v4l2` busca el driver del sensor al cargarse:

```bash
sudo modprobe vin_io
sudo insmod imx296_mipi.ko
sudo modprobe vin_v4l2
```

CAM2 aparece como `/dev/video8`. CAM1 debería aparecer como `/dev/video0`, pero aún no se ha probado con una cámara conectada. Para capturar 10 cuadros:

```bash
v4l2-ctl -d /dev/video8 --set-input=0 \
  --set-fmt-video=width=1456,height=1088,pixelformat=RG10 \
  --stream-mmap=8 --stream-count=10 --stream-to=cam2.raw
```

Al grabar cuadros conviene usar 8 búferes o más (`--stream-mmap=8`): con 4, escribir cada cuadro de 3 MB a veces se atrasa y se pierde uno de cada ~120 (pasa igual con el VIN original).

> **Importante:** `--set-input=0` (`VIDIOC_S_INPUT`) tiene que ir en el mismo proceso que captura. Sin eso, el VIN de la imagen provoca un *kernel panic*; con el parche de `driver/vin/`, `STREAMON` devuelve un error (ver más abajo).

VIN entrega cada píxel en 16 bits little-endian, alineado a la derecha (valores de 0 a 1023, 2912 bytes por línea). Como VIN no tiene el formato Y10, la cámara mono se declara como `SRGGB10`, que usa el mismo empaquetado RAW10, y los datos se leen como gris.

### Modos

VIN elige el modo según el tamaño pedido en `--set-fmt-video`: el más chico que sea mayor o igual.

| Modo | Cómo | fps por defecto | Exposición máxima | Bytes por cuadro |
|---|---|---|---|---|
| 1456×1088 | completo | 60,00 (VMAX 1125) | 16,6 ms (33,3 ms a 30 fps) | 3 168 256 |
| 1456×544 | recorte vertical centrado (filas 272 a 815) | 100,00 (VMAX 675) | 9,94 ms | 1 584 128 |
| 728×544 | binning 2×2: campo completo a media resolución, ~4 veces más señal | 100,00 (VMAX 675) | 9,94 ms | 800 768 (con relleno al final) |

La línea dura siempre 14,815 µs: por eso un recorte horizontal no da más fps, y sí uno vertical. Cada cuadro dura VMAX líneas, con VMAX = alto + VBLANK, y los fps son 67 500 / VMAX. Con `vertical_blanking` se bajan los fps y se alarga la exposición máxima, pero no se pueden subir por encima de los del modo: VIN dimensiona sus búferes y su tasa de envío según esos fps. El mismo VBLANK se mantiene mientras no cambie el modo; al cambiarlo, vuelve al valor del modo nuevo. Por eso, si se quiere otro VBLANK, primero se elige el modo y después se fija el control.

En 728×544, VIN agrega 8704 bytes de relleno al final de cada cuadro (`Size Image` = 800 768): los conversores de `scripts/` aceptan el tamaño real del cuadro (`--tam-cuadro`).

### Controles del sensor

Se manejan en el subdispositivo del sensor (`/dev/v4l-subdev0` con CAM1 vacío; `sudo bash captura.sh controles` lo encuentra), en vivo o antes de capturar. Fuera de transmisión solo se guardan y se aplican al arrancar. Los controles de exposición y ganancia del nodo de video, en cambio, van al ISP de VIN (`isp_used = 1` en el device tree) y no al sensor.

| Control | Unidad y rango | Notas |
|---|---|---|
| `exposure` | líneas de 14,815 µs, de 1 a VMAX − 4 | Se recorta al aplicarse. Con SHS1 = VMAX − exposición menor que 4, el sensor entrega cuadros negros (medido) |
| `analogue_gain` | pasos de 0,1 dB, de 0 a 480 (48 dB) | Exposición y ganancia cambian en el mismo cuadro (bloque REGHOLD) |
| `vertical_blanking` | líneas | Ver "Modos" |
| `horizontal_flip`, `vertical_flip` | 0/1 | Por defecto 1 y 1 (el sensor va montado invertido) |
| `test_pattern` | menú de 9 patrones | Los valores salen exactos (0x100 y 0x300): sirve para probar la cadena sin luz |
| `temperature_mc` | milésimas de °C, solo lectura | Se mide mientras transmite; si no, queda la última medida |
| `gain` | Q4 (16 = 1×), solo lectura | La ganancia lineal, para VIN |
| `horizontal_blanking`, `pixel_rate` | solo lectura | Información de temporización |

```bash
SD=/dev/v4l-subdev0
v4l2-ctl -d $SD -c exposure=20,analogue_gain=180     # 0,3 ms y 18 dB, en el mismo cuadro
v4l2-ctl -d /dev/video8 --set-input=0 --set-fmt-video=width=1456,height=1088,pixelformat=RG10
v4l2-ctl -d $SD -c vertical_blanking=1162            # 30 fps con el modo completo ya elegido
v4l2-ctl -d $SD -C temperature_mc
```

Los cambios en vivo entran unos 2 cuadros después de pedirlos. Al bajar la ganancia, el nivel de negro automático del sensor tarda unos 8 cuadros en reacomodarse (~1 cuenta de 1023).

### Parámetros del módulo

Son los valores iniciales de los controles y solo se leen al cargar el módulo (desde v3, después se usan los controles). Un valor fuera de rango se rechaza en `insmod`.

| Parámetro | Por defecto | Qué hace |
|---|---|---|
| `rotation` | 180 | 180 = espejo horizontal y vertical en el sensor, que va montado invertido en la cámara GS (igual que `rotation = <180>` en el overlay de Raspberry Pi). 0 = sin espejo |
| `exposure_us` | 10000 | Exposición inicial en µs, de 1 a 1 000 000, en pasos de una línea (14,8 µs) |
| `gain_db10` | 0 | Ganancia analógica inicial en pasos de 0,1 dB, de 0 a 480 |
| `trigger_mode` | 0 | 0 o 1, se aplica al arrancar cada captura. 1 = disparo externo por XTR, como en el driver de Raspberry Pi (sin probar) |

### Scripts

- `scripts/placa/captura.sh`: carga y captura paso a paso, revisando la microSD antes y después. Por ejemplo, `sudo CAM=2 bash captura.sh carga` y luego `controles`, `captura` o `video <nombre> [cuadros]`; el modo se elige con `ANCHO` y `ALTO` (`sudo ANCHO=728 ALTO=544 bash captura.sh captura`). El video se graba en `/dev/shm`, porque a 60 fps son unos 190 MB/s, más de lo que aguanta la microSD.
- `scripts/placa/prueba_modos.sh`: mide los fps de cada modo (y a 30 fps con VBLANK) con las marcas de tiempo, los compara con los calculados y comprueba que la exposición máxima dé imagen.
- `scripts/placa/analizar_raw.py` (numpy): media, mínimo y máximo por cuadro, y los valores distintos (`--valores`, para el patrón de prueba).
- `scripts/placa/raw16_a_gris8.py` (numpy) y `scripts/pc/gris8_a_mp4.sh` (ffmpeg): video MP4 y fotos.
- `scripts/pc/raw_a_png.py` (sin dependencias): RAW → PNG, `py -3 raw_a_png.py cam2.raw 1456 1088 cam2`.
- `scripts/placa/info_placa.sh` y `scripts/placa/prueba_i2c.sh`: inventario y prueba de I2C sin VIN. Para CAM2, `sudo ./prueba_i2c.sh 9 PE10`; para CAM1, `sudo ./prueba_i2c.sh 11 PE6`.
- `scripts/placa/prueba_termica.sh`: minutos en reposo y luego captura continua a 60 fps, con las temperaturas cada 5 s en un CSV y un resumen de cuadros y saltos de secuencia. Conviene lanzarlo con `sudo systemd-run --unit=prueba-termica bash prueba_termica.sh 10 10`, para que siga aunque se corte el SSH. Con disipador: ~45 °C de media capturando, 36 000 cuadros sin saltos.
- `scripts/pc/serie_grabar.ps1` y `scripts/pc/esperar_ssh.ps1` (Windows): graban la consola serie con la hora UTC de cada línea y miden cuánto tarda en volver el SSH después de un reinicio.

## Problemas conocidos de VIN

Estos fallos son del módulo `vin_v4l2` de la imagen, no de este driver. Los dos terminan en *kernel panic* por puntero NULL:

1. **Leer controles antes de `VIDIOC_S_INPUT`** (`v4l2-ctl --list-ctrls`) cuando CAM1 no tiene un sensor válido: `vin_g_volatile_ctrl` consulta el sensor 0.
2. **`STREAMON` sin `S_INPUT` en la misma sesión.** VIN habilita los enlaces del grafo de medios en `S_INPUT` y los deshabilita al cerrar el dispositivo. Sin enlaces, el formato no llega al CSI y `sunxi_csi_subdev_s_stream` usa un formato NULL.

Para evitarlos, hay que hacer `S_INPUT` justo después de abrir el dispositivo, en el mismo proceso que captura: con `v4l2-ctl`, `--set-input=0` en la misma invocación; en una aplicación propia, `VIDIOC_S_INPUT` antes de cualquier otro ioctl.

**Parche:** `driver/vin/` corrige `vin_v4l2.ko` (es un módulo: no hay que tocar el kernel) para que esos dos casos devuelvan un error del ioctl en vez del *panic*: `ENODEV` en los controles y `EINVAL` en `STREAMON`. `S_INPUT` en la misma sesión sigue siendo necesario para capturar. Se instala en `/lib/modules/$(uname -r)/updates/` sin pisar el original; ver `driver/vin/README.md`. El release trae el `.ko` ya compilado.

Otros detalles:

- No recargar `vin_v4l2` sin recargar también `imx296_mipi`.
- VIN imprime avisos como `scaler get_selection error` o `video8 has already stream off`; no afectan la captura.
- Las marcas de tiempo de los buffers son monotónicas y se toman al final del cuadro (`ts-src-eof`). El centro de la exposición queda unos 16,1 ms antes (lectura de 1088 líneas de 14,8 µs) más media exposición.
- Para depurar sirven `sudo dmesg -n 8` y una consola serie (pines 6, 8 y 10, 115200 baudios). Con el nivel de log por defecto, un *panic* de VIN no se ve y parece un cuelgue.

## Robustez de la placa

Con la configuración de fábrica de la imagen, un *kernel panic* como los de VIN deja la placa colgada hasta desenchufarla, y no queda registro de lo que pasó: el journal y `/var/log` viven en RAM. `config/` trae lo necesario para que la placa se reinicie sola y conserve el registro. No toca el kernel ni el device tree.

| Qué | Dónde | Por qué |
|---|---|---|
| Reiniciar 10 s después de un *panic* | `extraargs=panic=10` en `/boot/orangepiEnv.txt` | La imagen trae `CONFIG_PANIC_TIMEOUT=0`, y por línea de comandos vale desde el primer instante del arranque |
| Watchdog por hardware | `config/etc/systemd/system.conf.d/90-watchdog.conf` | Cubre los cuelgues que no llegan a *panic*. **16 s es el máximo del hardware** (`sunxi-wdt`): con un valor mayor, systemd 249 deja el watchdog armado sin atenderlo y la placa se reinicia a los 16 s de cada arranque |
| Journal en la microSD | `config/etc/systemd/journald.conf.d/90-persistente.conf` y `ENABLED=false` en `/etc/default/orangepi-ramlog` | La imagen guarda el journal en RAM y `/var/log` en una zram que solo se copia a la microSD una vez al día o al apagar limpio |
| Commit de ext4 cada 5 s | `commit=600` → `commit=5` en `/etc/fstab` | Con 600, un corte puede perder los últimos 10 minutos de escrituras |
| SysRq solo para sync | `config/etc/sysctl.d/90-sysrq.conf` (`kernel.sysrq = 16`) | La imagen trae 176, que permite reiniciar y apagar, y el kernel acepta SysRq por la consola serie: un BREAK (por ejemplo, al desconectarse o suspenderse el adaptador USB-serie) más un carácter cualquiera pueden reiniciar la placa. `/proc/sysrq-trigger` sigue funcionando |
| Registro de arranques y temperaturas | `config/usr/local/sbin/registro-placa` y los servicios `registro-arranque` y `registro-temperatura` | Anota en `/var/log/arranques.log` si el arranque anterior terminó limpio y, si no, guarda el final de su journal en `/var/log/arranques/`. Escribe una línea de temperaturas en el journal cada 30 s |

Instalación, desde la carpeta del repositorio copiada en la placa:

```bash
sudo cp -p /etc/fstab /etc/fstab.bak && sudo cp -p /boot/orangepiEnv.txt /boot/orangepiEnv.txt.bak
sudo sed -i 's/,commit=600,/,commit=5,/' /etc/fstab
sudo findmnt --verify && sudo mount -o remount /        # relee fstab: si falla, no reiniciar
sudo sh -c 'echo extraargs=panic=10 >> /boot/orangepiEnv.txt'  # si ya hay "extraargs=", sumarlo a esa línea
sudo install -D -m 644 config/etc/systemd/system.conf.d/90-watchdog.conf /etc/systemd/system.conf.d/90-watchdog.conf
sudo install -D -m 644 config/etc/systemd/journald.conf.d/90-persistente.conf /etc/systemd/journald.conf.d/90-persistente.conf
sudo install -m 644 config/etc/sysctl.d/90-sysrq.conf /etc/sysctl.d/ && sudo sysctl -p /etc/sysctl.d/90-sysrq.conf
sudo install -m 755 config/usr/local/sbin/registro-placa /usr/local/sbin/
sudo install -m 644 config/etc/systemd/system/registro-*.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable registro-arranque registro-temperatura
sudo /usr/lib/orangepi/orangepi-ramlog write             # copia a la microSD lo que hay en la zram
sudo sed -i 's/^ENABLED=true$/ENABLED=false/' /etc/default/orangepi-ramlog
sudo reboot
```

Después, `sudo bash scripts/placa/estado_robustez.sh` muestra el estado de cada defensa. Algunos detalles:

- **Leer el journal de un arranque anterior:** `journalctl -b <boot id>`, con el ID que anota `/var/log/arranques.log`. Después de un corte, `journalctl -b -1` puede no encontrarlo: la placa no tiene RTC, la hora vuelve a la última que guardó `fake-hwclock` y journalctl se confunde con arranques superpuestos en el tiempo.
- Lo que se escribe en los últimos ~10 s antes de un *panic* se pierde (`SyncIntervalSec=10s`); el mensaje del *panic* mismo solo se ve por la consola serie (la imagen no trae pstore ni netconsole).
- Para ver el watchdog no conviene usar `wdctl`: este kernel no trae `CONFIG_WATCHDOG_SYSFS`, así que `wdctl` abre el dispositivo, y eso lo arma.
- No activar `kernel.panic_on_warn`: la imagen genera un WARNING inofensivo en cada arranque (driver USB de Allwinner, al escribir `otg_role`), y la placa entraría en *panic* siempre.
- Para probarlo, con la consola serie conectada y después de un `sync`: `echo c | sudo tee /proc/sysrq-trigger` provoca un *panic*.

## Precauciones de hardware

- La entrada de disparo externo (XTR) de la cámara GS es de **1,8 V**: nunca hay que aplicarle 3,3 V directamente. Raspberry Pi sugiere un divisor de 1,5 kΩ / 1,8 kΩ ([documentación](https://www.raspberrypi.com/documentation/accessories/camera.html#external-trigger-on-the-gs-camera)).
- En esta placa, el I2C y los pines CAM_IO del conector trabajan a 3,3 V (medido), como en Raspberry Pi.

## Licencia y fuentes

GPL-2.0 (ver `LICENSE`). El driver se deriva de:

- `bsp/drivers/vin/modules/sensor/imx219.c` e `imx290_mipi.c` del [kernel de Orange Pi](https://github.com/orangepi-xunlong/linux-orangepi), rama `orange-pi-6.6-sun60iw2` (Allwinner, GPL-2.0-or-later): integración con VIN y manejo de dos instancias.
- `drivers/media/i2c/imx296.c` del kernel principal de Linux (Laurent Pinchart, GPL-2.0) y su versión en el [kernel de Raspberry Pi](https://github.com/raspberrypi/linux), rama `rpi-6.6.y`: valores de registros y modo de disparo externo.
