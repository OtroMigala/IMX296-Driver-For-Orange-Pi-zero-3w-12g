#!/bin/bash
# Prueba térmica: minutos en reposo y luego captura continua a 60 fps desde CAM2, con las
# temperaturas cada 5 s en un CSV. Al final resume la captura: cuadros, saltos de secuencia e
# intervalo entre cuadros.
#
# Uso: sudo bash prueba_termica.sh [min_reposo] [min_captura]     (por defecto 10 y 10)
#   Mejor fuera de la sesión SSH, para que siga aunque se corte la conexión:
#   sudo systemd-run --unit=prueba-termica bash ~/port/prueba_termica.sh
#
# Salida en la carpeta del script: termica_<fecha>.csv, termica_<fecha>_carga.log y
# termica_<fecha>_captura.txt. El registro detallado de v4l2-ctl va a /dev/shm (36 000 líneas).
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
REPOSO=${1:-10}
CAPTURA=${2:-10}
BASE=$DIR/termica_$(date -u +%Y%m%d_%H%M)
FASE=$DIR/.fase_termica
V4L2_LOG=/dev/shm/termica_v4l2.log

registrar() {
	echo "utc,uptime_s,fase,$(sed 's/_thermal_zone$//; s/_zone$//' /sys/class/thermal/thermal_zone*/type | paste -sd,)" > "$BASE.csv"
	while :; do
		echo "$(date -u +%T),$(cut -d' ' -f1 /proc/uptime),$(cat "$FASE"),$(awk '{printf "%.1f\n", $1 / 1000}' /sys/class/thermal/thermal_zone*/temp | paste -sd,)" >> "$BASE.csv"
		sleep 5
	done
}

# Con el nivel de log por defecto, un panic no se ve por la consola serie.
dmesg -n 8
echo reposo > "$FASE"
registrar &
REG=$!
trap 'kill $REG 2>/dev/null; sync' EXIT
logger -t prueba-termica "inicio: $REPOSO min en reposo, luego $CAPTURA min capturando"
sleep $((REPOSO * 60))

echo carga > "$FASE"
CAM=2 bash "$DIR/captura.sh" carga > "${BASE}_carga.log" 2>&1 || true
sync

echo captura > "$FASE"
N=$((CAPTURA * 60 * 60))
logger -t prueba-termica "captura de $N cuadros"
# --set-input en la misma invocación que captura (bug de VIN, ver captura.sh).
timeout $((CAPTURA * 60 + 120)) v4l2-ctl -d /dev/video8 --set-input=0 \
	--set-fmt-video=width=1456,height=1088,pixelformat=RG10 \
	--stream-mmap=4 --stream-count=$N --verbose > "$V4L2_LOG" 2>&1
rc=$?
echo fin > "$FASE"
{
	echo "v4l2-ctl rc=$rc, cuadros pedidos $N"
	awk '/cap dqbuf/ {
		for (i = 1; i <= NF; i++) { if ($i == "seq:") s = $(i + 1); if ($i == "ts:") t = $(i + 1) }
		n++; if (n == 1) { s0 = s; t0 = t } else { d = (t - tp) * 1000; if (d > dmax) dmax = d; if (dmin == "" || d < dmin) dmin = d; if (s != sp + 1) saltos++ }
		sp = s; tp = t }
		END { printf "cuadros %d, seq %d..%d, saltos de secuencia %d\n", n, s0, sp, saltos + 0
		      printf "intervalo medio %.4f ms (%.3f fps), min %.3f, max %.3f ms\n", (tp - t0) * 1000 / (n - 1), (n - 1) / (tp - t0), dmin, dmax }' "$V4L2_LOG"
	echo "--- dmesg (sin WiFi)"; dmesg | grep -viE "rwnx|aic|wlan|cfg80211|scanu" | tail -8
} > "${BASE}_captura.txt"
logger -t prueba-termica "fin: $(head -2 "${BASE}_captura.txt" | tail -1)"
sync
