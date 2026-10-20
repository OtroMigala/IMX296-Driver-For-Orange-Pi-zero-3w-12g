#!/bin/bash
# Estado de las defensas de la placa ante fallos (solo lectura): reinicio tras un panic,
# watchdog, sistema de archivos, journal persistente y temperatura.
#
# Uso: sudo bash estado_robustez.sh
#
# No usa wdctl: sin CONFIG_WATCHDOG_SYSFS, wdctl abre /dev/watchdog para leerlo, y abrirlo lo arma.
t() { echo; echo "== $*"; }

t "panic"
echo "kernel.panic = $(cat /proc/sys/kernel/panic)   (10 = reinicia 10 s después de un panic; 0 = queda colgada)"
echo "panic_on_oops = $(cat /proc/sys/kernel/panic_on_oops)   panic_on_warn = $(cat /proc/sys/kernel/panic_on_warn)"
echo "línea de comandos: $(tr ' ' '\n' < /proc/cmdline | grep -E '^(panic|oops|loglevel)=' | tr '\n' ' ')"
echo "kernel.sysrq = $(cat /proc/sys/kernel/sysrq)   (16 = solo sync por teclado/serie; 176 permite reiniciar y apagar con un BREAK)"

t "watchdog"
systemctl show -p RuntimeWatchdogUSec -p RebootWatchdogUSec | tr '\n' ' '; echo
# systemd 249 abre el nodo clásico /dev/watchdog (10:130); /dev/watchdog0 es el mismo driver.
fuser -v /dev/watchdog /dev/watchdog0 2>&1 | tail -n +2 | grep . || echo "nadie tiene abierto el watchdog (desarmado)"
journalctl -b -q --no-pager -o cat 2>/dev/null | grep -iE "hardware watchdog" | tail -2

t "sistema de archivos raíz"
dev=$(findmnt -no SOURCE / | sed 's|/dev/||')
echo "fstab: $(awk '$2 == "/" {print $4}' /etc/fstab)"
echo "en uso: $(tr ',' '\n' < /proc/fs/ext4/$dev/options | grep -E '^commit=')   (segundos entre commits del journal de ext4)"

t "logs"
echo "orangepi-ramlog: $(grep -E '^ENABLED=' /etc/default/orangepi-ramlog)"
echo "/var/log en: $(findmnt -no SOURCE,FSTYPE --target /var/log)"
echo "journald: Storage=$(systemd-analyze cat-config systemd/journald.conf 2>/dev/null | grep -E '^Storage=' | tail -1 | cut -d= -f2)" \
	"en $(ls -d /var/log/journal/*/ /run/log/journal/*/ 2>/dev/null | head -1)"
journalctl --disk-usage 2>&1
# No se usa --list-boots: sin RTC, la hora retrocede tras un corte y journalctl deja de ver
# arranques. Para leer uno: journalctl -b <boot id>, con el ID que anota /var/log/arranques.log.
echo "arranques en el journal: $(journalctl -F _BOOT_ID 2>/dev/null | wc -l)"
[ -f /var/log/arranques.log ] && { echo "/var/log/arranques.log:"; tail -3 /var/log/arranques.log; }

t "temperatura y ventilador"
paste <(sed 's/_thermal_zone$//; s/_zone$//' /sys/class/thermal/thermal_zone*/type) \
	<(awk '{printf "%.1f °C\n", $1 / 1000}' /sys/class/thermal/thermal_zone*/temp) | head -5
[ -d /sys/class/gpio/gpio101 ] && echo "GPIO 101 (PD5, ventilador del conector de la placa) = $(cat /sys/class/gpio/gpio101/value)"
echo "uptime: $(uptime -p)"
