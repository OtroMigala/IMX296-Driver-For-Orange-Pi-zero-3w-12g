#!/bin/sh
# Foto de lo que toca el paquete imx296-vin, para comprobar que desinstalarlo deja el sistema
# como estaba: una foto antes de instalar, otra después de purgar, y diff entre las dos.
# Solo lee; no hace falta sudo. No incluye fechas (depmod reescribe los índices aunque no
# cambien) ni los módulos cargados (eso depende del arranque, no de los archivos).
#
# Uso: sh estado_paquete.sh > antes.txt   ...   sh estado_paquete.sh > despues.txt
#      diff antes.txt despues.txt
K=/lib/modules/$(uname -r)
echo "== kernel $(uname -r)"
echo "== índices de módulos"
(cd "$K" && md5sum modules.*)
for d in "$K/updates" /lib/modprobe.d /etc/modprobe.d /usr/lib/imx296-vin /usr/share/imx296-vin \
	/usr/share/doc/imx296-vin /var/lib/imx296-vin /boot/overlay-user; do
	echo "== $d"
	if [ -d "$d" ]; then
		find "$d" -mindepth 1 -printf '%M %u:%g %P\n' | sort
		find "$d" -type f -exec md5sum {} + | sed "s|  $d/|  |" | sort -k2
	else
		echo "(no existe)"
	fi
done
echo "== unidades systemd"
find /lib/systemd/system /etc/systemd/system -name 'imx296*' -printf '%M %p -> %l\n' | sort
systemctl is-enabled imx296-camaras.service 2>&1
echo "== programas"
ls -l /usr/bin/imx296* /usr/sbin/imx296* 2>&1
echo "== dpkg"
dpkg-query -W -f='${Package} ${Version} ${Status}\n' imx296-vin 2>&1
ls /var/lib/dpkg/info/imx296-vin.* 2>&1
echo "== arranque"
md5sum /boot/orangepiEnv.txt
grep -E '^(overlays|user_overlays|extraargs)=' /boot/orangepiEnv.txt
