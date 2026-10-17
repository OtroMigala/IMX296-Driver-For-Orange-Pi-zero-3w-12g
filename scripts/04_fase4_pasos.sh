#!/bin/bash
# Pasos de la Fase 4, uno por vez, revisando la salud de la microSD antes y después.
# Uso: sudo [CAM=1|2] bash paso.sh <carga|info|formatos|getfmt|ctrls|captura [nombre]|estado>
#   CAM=1 -> /dev/video0 (conector CAM1), CAM=2 -> /dev/video8 (CAM2, por defecto)
CAM=${CAM:-2}
if [ "$CAM" = 1 ]; then DEV=/dev/video0; else DEV=/dev/video8; fi
salud() {
  echo "-- microSD: errores mmc/EXT4 en dmesg: $(dmesg | grep -ciE 'mmc[0-9]*:.*(err|timeout|fail)|I/O error|blk_update_request|EXT4-fs (error|warning)')"
  touch /home/orangepi/port/.prueba_escritura && sync && echo "-- escritura + sync en microSD: OK"
}
echo "== paso: $1   ($(date +%T), temp $(cat /sys/class/thermal/thermal_zone0/temp))"
salud
case "$1" in
  carga)
    modprobe vin_io; echo "vin_io rc=$?"
    insmod /home/orangepi/port/imx296_mipi.ko; echo "insmod rc=$?"
    timeout 90 modprobe vin_v4l2; echo "vin_v4l2 rc=$?"
    sleep 2
    ls /dev/video* 2>&1
    # Bug de VIN: antes de VIDIOC_S_INPUT, sensor_sel = 0 (CAM1 sin sensor) y leer
    # controles hace NULL deref en vin_g_volatile_ctrl. S_INPUT deja sensor_sel = 2.
    timeout 20 v4l2-ctl -d $DEV --set-input=0; echo "set-input rc=$?"
    dmesg | grep -iE "imx296|registering|No such device|WARNING" | tail -6 ;;
  info)     timeout 20 v4l2-ctl -d $DEV --info; echo "rc=$?" ;;
  formatos) timeout 20 v4l2-ctl -d $DEV --list-formats-ext | tail -5; echo "rc=$?" ;;
  getfmt)   timeout 20 v4l2-ctl -d $DEV --get-fmt-video | head -4; echo "rc=$?" ;;
  ctrls)    timeout 20 v4l2-ctl -d $DEV --set-input=0
            timeout 30 v4l2-ctl -d $DEV --list-ctrls; echo "rc=$?" ;;
  captura)
    # Uso: paso.sh captura [nombre]   (parámetros actuales en /sys/module/imx296_mipi/parameters)
    OUT=/home/orangepi/port/${2:-cam${CAM}_10cuadros}.raw
    echo "parametros: $(for p in rotation exposure_us gain_db10 trigger_mode; do echo -n "$p=$(cat /sys/module/imx296_mipi/parameters/$p) "; done)"
    rm -f $OUT
    # --set-input en la MISMA sesión: VIN habilita los enlaces sensor->mipi y csi->tdm/isp
    # en S_INPUT y los deshabilita al cerrar el dispositivo. Sin enlaces, S_FMT no llega al
    # CSI (csi_fmt = NULL) y STREAMON cae en sunxi_csi_subdev_s_stream (NULL deref 0x4).
    timeout 60 v4l2-ctl -d $DEV --set-input=0 \
      --set-fmt-video=width=1456,height=1088,pixelformat=RG10 \
      --stream-mmap=4 --stream-count=10 --stream-to=$OUT --verbose 2>&1 | tail -25
    echo "captura rc=${PIPESTATUS[0]}"
    sync; ls -l $OUT 2>&1
    timeout 20 v4l2-ctl -d $DEV --get-fmt-video | head -12 ;;
  video)
    # Uso: paso.sh video <nombre> [cuadros]  -> /dev/shm/<nombre>.raw (RAM: a 60 fps son
    # ~190 MB/s, más de lo que soporta la microSD) y /dev/shm/<nombre>.gray8 (8 bits).
    N=${3:-180}
    OUT=/dev/shm/${2:-video}.raw
    echo "parametros: $(for p in rotation exposure_us gain_db10; do echo -n "$p=$(cat /sys/module/imx296_mipi/parameters/$p) "; done)"
    rm -f $OUT
    timeout 90 v4l2-ctl -d $DEV --set-input=0 \
      --set-fmt-video=width=1456,height=1088,pixelformat=RG10 \
      --stream-mmap=8 --stream-count=$N --stream-to=$OUT --verbose 2>&1 | grep -E "fps:" | tail -3
    echo "video rc=${PIPESTATUS[0]}"
    ls -l $OUT
    python3 /home/orangepi/port/raw16_a_gris8.py $OUT ${OUT%.raw}.gray8 ;;
  estado)   lsmod | grep -iE "vin|imx"; ls /dev/video* 2>&1 ;;
esac
salud
echo "-- últimos mensajes del kernel (sin WiFi):"
dmesg | grep -viE "rwnx|aic|wlan|cfg80211|scanu" | tail -6
