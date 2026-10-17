#!/usr/bin/env bash
# En la placa: inventario de kernel, módulos, buses I2C, GPIO y nodos de cámara del
# device tree en vivo. Genera info_placa.txt en la carpeta actual.
# Uso: sudo ./info_placa.sh
OUT="info_placa.txt"
{
  echo "### Fecha"; date
  echo; echo "### Kernel"; uname -a
  echo; echo "### Sistema"; cat /etc/os-release 2>/dev/null
  echo; echo "### Headers del kernel (para compilar en la placa)"; ls -d /usr/src/linux-headers-* 2>/dev/null || echo "No hay headers instalados"
  echo; echo "### Config del kernel (cámaras)"
  # [^S]CSI evita que aparezcan todas las opciones de SCSI
  PAT='SENSOR_|VIDEO_IMX|VIN|[^S]CSI|CCI|MODVERSIONS|MODULE_SIG'
  if [ -r /proc/config.gz ]; then zcat /proc/config.gz | grep -E "$PAT" | grep -v "^# CONFIG_SENSOR_.* is not set"
  elif [ -r "/boot/config-$(uname -r)" ]; then grep -E "$PAT" "/boot/config-$(uname -r)" | grep -v "^# CONFIG_SENSOR_.* is not set"
  else echo "No se encontró la config del kernel"; fi
  echo; echo "### Módulos de cámara disponibles"; find "/lib/modules/$(uname -r)" \( -name "vin*" -o -name "*imx*" -o -name "sensor*" \) 2>/dev/null
  echo; echo "### Módulos cargados"; lsmod | grep -iE "vin|imx|sensor|cci" || echo "Ninguno relacionado"
  echo; echo "### Buses I2C"; ls -l /dev/i2c-* 2>/dev/null; command -v i2cdetect >/dev/null && i2cdetect -l || echo "Falta i2c-tools (sudo apt install i2c-tools)"
  echo; echo "### GPIO (líneas PE)"; command -v gpioinfo >/dev/null && gpioinfo 2>/dev/null | grep -iE "gpiochip|PE" | head -60 || echo "Falta gpiod (sudo apt install gpiod)"
  echo; echo "### Video"; ls -l /dev/video* 2>/dev/null || echo "Sin /dev/video*"
  echo; echo "### Device tree en vivo: nodos de sensores"
  # /proc/device-tree es un enlace simbólico: se usa la ruta real para que find entre
  for n in $(find /sys/firmware/devicetree/base \( -name "sensor@*" -o -name "vinc@*" \) 2>/dev/null | sort); do
    case "$n" in *vinc@*) [ "$(tr -d '\0' < "$n/status" 2>/dev/null)" = okay ] || continue;; esac
    echo "-- ${n#/sys/firmware/devicetree/base}"
    for p in "$n"/*; do
      b=$(basename "$p")
      case "$b" in *mname|*twi_cci_id|*twi_addr|*mclk_id|*isp_used|*fmt|*pwdn|*reset|*_sel|work_mode|status)
        # Texto si tiene letras; si no, bytes en hex (celdas de 32 bits big-endian)
        if tr -d '\0' < "$p" | grep -q '[[:alpha:]]'; then v=$(tr -d '\0' < "$p"); else v=$(od -An -tx1 "$p" | tr -s ' '); fi
        printf "   %-24s %s\n" "$b" "$v";;
      esac
    done
  done
  echo; echo "### dmesg (vin/sensor/cci)"; dmesg | grep -iE "vin|sensor|cci|csi|imx" | tail -80
} > "$OUT" 2>&1
echo "Inventario guardado en $OUT"
