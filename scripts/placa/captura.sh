#!/bin/bash
# Carga del driver y captura en la placa, un paso por vez, revisando la salud de la microSD
# antes y después de cada paso (tras un kernel panic se pierden las escrituras recientes).
#
# Uso: [sudo] [CAM=1|2] [ANCHO=.. ALTO=..] [SALIDA=dir] [PORT_DIR=dir] bash captura.sh <paso> [arg]
#   Con el paquete imx296-vin: imx296-captura <paso> [arg]. sudo solo hace falta para carga
#   (si VIN no está cargado) y para revisar dmesg; para capturar basta con el grupo video.
#   carga                    vin_io -> imx296_mipi -> vin_v4l2, y S_INPUT (con el paquete
#                            imx296-vin, el servicio imx296-camaras ya los carga al arrancar)
#   info | formatos | getfmt | ctrls | estado
#   controles                controles del sensor en su /dev/v4l-subdevN (exposición, ganancia...)
#   captura [nombre]         10 cuadros RAW10 a $SALIDA/<nombre>.raw
#   video <nombre> [cuadros] cuadros a /dev/shm/<nombre>.raw (RAM) y conversión a gris de 8 bits
#
#   CAM=1 -> /dev/video0 (conector CAM1), CAM=2 -> /dev/video8 (CAM2, por defecto)
#   ANCHO x ALTO: modo del sensor, 1456x1088 (por defecto, 60 fps), 1456x544 (recorte) o
#   728x544 (binning), a 100 fps. Los fps se bajan con el control vertical_blanking.
#   SALIDA: carpeta de las capturas; por defecto, la actual.
#   PORT_DIR: carpeta con imx296_mipi.ko (si no está instalado en el sistema) y raw16_a_gris8.py.
#   Por defecto, la de este script (no $HOME: con sudo, Ubuntu cambia $HOME a /root).
CAM=${CAM:-2}
ANCHO=${ANCHO:-1456}
ALTO=${ALTO:-1088}
SALIDA=${SALIDA:-$PWD}
PORT_DIR=${PORT_DIR:-$(cd "$(dirname "$0")" && pwd)}
if [ "$CAM" = 1 ]; then DEV=/dev/video0; SENSOR=imx296_mipi; else DEV=/dev/video8; SENSOR=imx296_mipi_2; fi
# Nodo del subdispositivo del sensor: el número depende de qué cámaras registró VIN.
nodo_sensor() {
  local f
  f=$(grep -lx "$SENSOR" /sys/class/video4linux/v4l-subdev*/name 2>/dev/null | head -1)
  [ -n "$f" ] && echo "/dev/$(basename "$(dirname "$f")")"
}
# Errores de almacenamiento nuevos durante el paso. Se compara contra el conteo inicial porque
# el arranque ya deja decenas de mensajes de sondeo (ranura eMMC vacía, WiFi por SDIO) que
# no son fallas de la microSD. Sin sudo no se puede leer dmesg (kernel.dmesg_restrict = 1).
salud() {
  local n f="$SALIDA/.prueba_escritura.$$"
  if dmesg > /dev/null 2>&1; then
    n=$(dmesg | grep -ciE 'mmc[0-9]*:.*(err|timeout|fail)|I/O error|blk_update_request|EXT4-fs (error|warning)')
    if [ -z "${N0:-}" ]; then N0=$n; else echo "-- microSD: $((n - N0)) errores nuevos durante el paso"; fi
  elif [ -z "${N0:-}" ]; then
    N0=-; echo "-- sin sudo no se puede leer dmesg: no se revisan los errores de la microSD"
  fi
  touch "$f" && sync && rm -f "$f" && echo "-- escritura + sync en microSD: OK"
}
echo "== paso: $1   ($(date +%T), temp $(cat /sys/class/thermal/thermal_zone0/temp))"
salud
case "$1" in
  carga)
    if grep -q '^vin_v4l2 ' /proc/modules; then
      # No se recarga: vin_v4l2 no se puede recargar sin recargar también el driver del sensor.
      echo "VIN ya está cargado (¿servicio imx296-camaras?): no se recarga"
    elif modinfo -n imx296_mipi >/dev/null 2>&1; then
      # Instalado en el sistema (paquete imx296-vin): el softdep carga vin_io e imx296_mipi.
      timeout 90 modprobe vin_v4l2; echo "modprobe vin_v4l2 rc=$?"
    else
      modprobe vin_io; echo "vin_io rc=$?"
      insmod "$PORT_DIR/imx296_mipi.ko"; echo "insmod rc=$?"
      timeout 90 modprobe vin_v4l2; echo "vin_v4l2 rc=$?"
    fi
    sleep 2
    ls /dev/video* 2>&1
    # Bug de VIN: antes de VIDIOC_S_INPUT, sensor_sel = 0 (CAM1 sin sensor) y leer
    # controles hace NULL deref en vin_g_volatile_ctrl (con el parche de driver/vin/, error
    # ENODEV). S_INPUT deja sensor_sel = 2.
    timeout 20 v4l2-ctl -d $DEV --set-input=0; echo "set-input rc=$?"
    dmesg 2>/dev/null | grep -iE "imx296|registering|No such device|WARNING" | tail -6 ;;
  info)     timeout 20 v4l2-ctl -d $DEV --info; echo "rc=$?" ;;
  formatos) timeout 20 v4l2-ctl -d $DEV --list-formats-ext | tail -5; echo "rc=$?" ;;
  getfmt)   timeout 20 v4l2-ctl -d $DEV --get-fmt-video | head -4; echo "rc=$?" ;;
  ctrls)    timeout 20 v4l2-ctl -d $DEV --set-input=0
            timeout 30 v4l2-ctl -d $DEV --list-ctrls; echo "rc=$?" ;;
  controles)
    SD=$(nodo_sensor)
    [ -n "$SD" ] || { echo "no hay subdispositivo $SENSOR (¿cargado el driver?)"; exit 1; }
    echo "sensor $SENSOR = $SD"
    timeout 20 v4l2-ctl -d "$SD" -l; echo "rc=$?" ;;
  captura)
    # Controles vigentes del sensor (exposición, ganancia, espejo, VBLANK)
    OUT="$SALIDA/${2:-cam${CAM}_10cuadros}.raw"
    SD=$(nodo_sensor)
    [ -n "$SD" ] && echo "controles: $(timeout 10 v4l2-ctl -d "$SD" -C exposure,analogue_gain,vertical_blanking,horizontal_flip,vertical_flip | tr '\n' ' ')"
    rm -f "$OUT"
    # --set-input en la MISMA sesión: VIN habilita los enlaces sensor->mipi y csi->tdm/isp
    # en S_INPUT y los deshabilita al cerrar el dispositivo. Sin enlaces, S_FMT no llega al
    # CSI (csi_fmt = NULL) y STREAMON cae en sunxi_csi_subdev_s_stream (NULL deref 0x4); con
    # el parche de driver/vin/, STREAMON devuelve EINVAL.
    # 8 búferes: con 4, escribir cada cuadro (3 MB) a veces se atrasa y se pierde uno.
    timeout 60 v4l2-ctl -d $DEV --set-input=0 \
      --set-fmt-video=width=$ANCHO,height=$ALTO,pixelformat=RG10 \
      --stream-mmap=8 --stream-count=10 --stream-to="$OUT" --verbose 2>&1 | tail -25
    echo "captura rc=${PIPESTATUS[0]}"
    sync; ls -l "$OUT" 2>&1
    timeout 20 v4l2-ctl -d $DEV --get-fmt-video | head -12 ;;
  video)
    # A 60 fps son ~190 MB/s, más de lo que soporta la microSD: se graba en RAM (/dev/shm)
    # y se convierte a gris de 8 bits (<nombre>.gray8) para armar el video en el PC.
    N=${3:-180}
    OUT=/dev/shm/${2:-video}.raw
    SD=$(nodo_sensor)
    [ -n "$SD" ] && echo "controles: $(timeout 10 v4l2-ctl -d "$SD" -C exposure,analogue_gain,vertical_blanking | tr '\n' ' ')"
    rm -f "$OUT"
    timeout 90 v4l2-ctl -d $DEV --set-input=0 \
      --set-fmt-video=width=$ANCHO,height=$ALTO,pixelformat=RG10 \
      --stream-mmap=8 --stream-count=$N --stream-to="$OUT" --verbose 2>&1 | grep -E "fps:" | tail -3
    echo "video rc=${PIPESTATUS[0]}"
    ls -l "$OUT"
    # VIN puede agregar relleno al final de cada cuadro (728x544: 800768 bytes en vez de 792064).
    [ -s "$OUT" ] && python3 "$PORT_DIR/raw16_a_gris8.py" "$OUT" "${OUT%.raw}.gray8" \
      "$ANCHO" "$ALTO" 0.7 $(( $(stat -c %s "$OUT") / N )) ;;
  estado)   lsmod | grep -iE "vin|imx"; ls /dev/video* 2>&1 ;;
  *)        echo "Paso desconocido: '$1'. Ver el encabezado del script."; exit 1 ;;
esac
salud
if dmesg > /dev/null 2>&1; then
  echo "-- últimos mensajes del kernel (sin WiFi):"
  dmesg | grep -viE "rwnx|aic|wlan|cfg80211|scanu" | tail -6
fi
