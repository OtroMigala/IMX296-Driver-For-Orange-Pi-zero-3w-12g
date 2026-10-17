#!/usr/bin/env bash
# Fase 2 (en la Orange Pi): ¿la IMX296 responde por I2C? No requiere driver.
# Uso: sudo ./03_hito1_i2c.sh <bus_i2c> <pin>     ej: sudo ./03_hito1_i2c.sh 11 PE6   (CAM1)
#                                                     sudo ./03_hito1_i2c.sh 9 PE10   (CAM2)
# Importante: arrancar SIN haber ejecutado "modprobe vin_v4l2".
set -u
BUS="${1:-11}"
PIN="${2:-PE6}"
ADDR=0x1a

for t in i2cdetect i2ctransfer gpioset; do
  command -v "$t" >/dev/null || { echo "Falta $t. Instala: sudo apt install i2c-tools gpiod"; exit 1; }
done

# PE6 -> 4*32+6 = 134 (numeración típica de Allwinner: letra A=0, B=1, ... E=4)
LETRA=$(echo "$PIN" | cut -c2); NUM=$(echo "$PIN" | cut -c3-)
IDX=$(( $(printf '%d' "'$LETRA") - 65 ))
LINEA=$(( IDX * 32 + NUM ))
echo "Pin $PIN -> línea $LINEA (verificar con: gpioinfo | grep -i $PIN)"

# Encender la cámara: libgpiod v1 y v2 tienen sintaxis distinta
# Solo la primera línea ("gpioset (libgpiod) v1.6.3"): las siguientes dicen "GPLv2" y engañaban al grep
if gpioset --version 2>&1 | head -1 | grep -qE "v2\.[0-9]"; then
  gpioset -c gpiochip0 "$LINEA=1" & GPID=$!
else
  gpioset --mode=signal gpiochip0 "$LINEA=1" & GPID=$!
fi
trap 'kill $GPID 2>/dev/null' EXIT
# Raspberry Pi espera 500 ms a que estabilicen los reguladores de la cámara
# (startup-delay-us = 500000 en imx296-overlay.dts); se usa 1 s por margen.
sleep 1

echo; echo "== Escaneo del bus $BUS (se busca 0x1a)"
i2cdetect -y -r "$BUS" 0x1a 0x1a

echo; echo "== Saliendo de standby (0x3000 = 0x00)"
if ! i2ctransfer -y "$BUS" w3@$ADDR 0x30 0x00 0x00; then
  echo "RESULTADO: no responde en 0x1a. Problema eléctrico (cable, 3,3 V, pin de encendido o bus equivocado)."
  exit 2
fi
sleep 0.05

echo "== Leyendo SENSOR_INFO (0x3148, 2 bytes, byte bajo primero)"
RAW=$(i2ctransfer -y "$BUS" w2@$ADDR 0x31 0x48 r2) || { echo "RESULTADO: responde pero no deja leer. Revisar el oscilador de 54 MHz de la cámara."; exit 3; }
echo "Bytes leídos: $RAW"
LO=$(echo "$RAW" | awk '{print $1}'); HI=$(echo "$RAW" | awk '{print $2}')
VAL=$(( (HI << 8) | LO ))
MODELO=$(( (VAL >> 6) & 0x1ff ))
printf "SENSOR_INFO = 0x%04x  ->  modelo = %d\n" "$VAL" "$MODELO"
case "$VAL" in
  $((0x4a00))) echo "RESULTADO: IMX296LQ (color). Hardware OK, pasar a la Fase 3." ;;
  $((0xca00))) echo "RESULTADO: IMX296LL (mono). Hardware OK, pasar a la Fase 3." ;;
  *) [ "$MODELO" -eq 296 ] && echo "RESULTADO: IMX296 detectada (variante 0x$(printf %04x $VAL))." || echo "RESULTADO: valor inesperado; anotar en la bitácora." ;;
esac

# Devolver el sensor a standby
i2ctransfer -y "$BUS" w3@$ADDR 0x30 0x00 0x01 >/dev/null 2>&1
