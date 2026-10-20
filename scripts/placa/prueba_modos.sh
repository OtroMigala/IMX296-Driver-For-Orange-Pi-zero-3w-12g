#!/bin/bash
# Prueba de los modos del driver (v3): fps medidos con las marcas de tiempo de VIN frente a los
# calculados (67500 / VMAX), saltos de secuencia, y que la exposición máxima (VMAX - 4 líneas)
# dé imagen y no cuadros negros.
#
# Uso: sudo bash prueba_modos.sh [cuadros] [ganancia_db10]     (por defecto 600 y 100)
#   sudo para leer dmesg; el driver y VIN tienen que estar cargados (captura.sh carga).
#   Deja la exposición en el máximo, la ganancia pedida y VBLANK en el valor del último modo.
N=${1:-600}
GAN=${2:-100}
DEV=/dev/video8
f=$(grep -lx imx296_mipi_2 /sys/class/video4linux/v4l-subdev*/name 2>/dev/null | head -1)
[ -n "$f" ] || { echo "no hay subdispositivo imx296_mipi_2 (¿cargado el driver?)"; exit 1; }
SD=/dev/$(basename "$(dirname "$f")")
DIR=$(cd "$(dirname "$0")" && pwd)

echo "sensor $SD; $N cuadros por modo; ganancia $GAN (0,1 dB); exposición pedida: máxima"
printf "%-10s %6s %9s %9s %8s %7s %9s %10s %8s\n" modo VMAX "fps calc" "fps med" "error" saltos "dt máx" "exp (us)" "media"
for modo in "1456 1088 0" "1456 544 0" "728 544 0" "1456 1088 1162"; do
	read -r W H VB <<< "$modo"
	# El modo se elige con S_FMT. Al cambiar de modo, el driver vuelve VBLANK al mínimo del modo
	# (sus fps nominales); si el modo no cambia, VBLANK conserva su valor: se fija siempre.
	timeout 20 v4l2-ctl -d $DEV --set-input=0 --set-fmt-video=width=$W,height=$H,pixelformat=RG10
	if [ "$VB" = 0 ]; then
		VB=$(v4l2-ctl -d "$SD" -l | awk '/vertical_blanking/ { for (i = 1; i <= NF; i++) if ($i ~ /^min=/) print substr($i, 5) }')
	fi
	v4l2-ctl -d "$SD" -c vertical_blanking="$VB"
	v4l2-ctl -d "$SD" -c exposure=1048574 -c analogue_gain="$GAN"
	VBL=$(v4l2-ctl -d "$SD" -C vertical_blanking | awk '{print $2}')
	VMAX=$((H + VBL))
	timeout 120 v4l2-ctl -d $DEV --set-input=0 --set-fmt-video=width=$W,height=$H,pixelformat=RG10 \
		--stream-mmap=8 --stream-count="$N" --verbose > /dev/shm/modo.log 2>&1
	timeout 30 v4l2-ctl -d $DEV --set-input=0 --set-fmt-video=width=$W,height=$H,pixelformat=RG10 \
		--stream-mmap=8 --stream-count=3 --stream-to=/dev/shm/modo.raw > /dev/null 2>&1
	TAM=$(( $(stat -c %s /dev/shm/modo.raw) / 3 ))
	MEDIA=$(python3 "$DIR/analizar_raw.py" /dev/shm/modo.raw "$W" "$H" --tam-cuadro "$TAM" | tail -1 | awk '{print $4}')
	EXPL=$(dmesg | grep "imx296_mipi\]stream ${W}x${H}" | tail -1 | sed -n 's/.*exposure \([0-9]*\) lines (\([0-9]*\) us).*/\2/p')
	# fps = 1 / pendiente de la recta tiempo-secuencia (mínimos cuadrados): usar solo la primera y
	# la última marca hace que una sola interrupción atrasada (~ms) mueva el resultado.
	awk -v w="$W" -v h="$H" -v vmax="$VMAX" -v e="$EXPL" -v m="$MEDIA" '
		/cap dqbuf/ { for (i = 1; i <= NF; i++) { if ($i == "seq:") s = $(i + 1); if ($i == "ts:") t = $(i + 1) }
			n++; if (n == 1) { s0 = s; t0 = t } else { d = (t - tp) * 1000; if (d > dm) dm = d; if (s != sp + 1) g++ }
			x = s - s0; y = t - t0; sx += x; sy += y; sxx += x * x; sxy += x * y
			sp = s; tp = t }
		END { calc = 67500 / vmax; med = (n * sxx - sx * sx) / (n * sxy - sx * sy)
			printf "%-10s %6d %9.3f %9.3f %7.3f%% %7d %8.3f ms %10s %8s\n", w "x" h, vmax, calc, med,
			       (med - calc) / calc * 100, g + 0, dm, e, m }' /dev/shm/modo.log
done
rm -f /dev/shm/modo.log /dev/shm/modo.raw
echo "criterio: error de fps dentro de ±0,1 %, 0 saltos, y media muy por encima del negro (~60)"
