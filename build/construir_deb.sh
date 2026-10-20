#!/usr/bin/env bash
# Arma el paquete imx296-vin_<versión>_arm64.deb en el contenedor Docker de build/.
#
# Uso: build/construir_deb.sh [versión]          (por defecto 3.1)
#   Antes: build/compilar_linux.sh imx296_mipi (o .\build\compilar.ps1 imx296_mipi) y
#   build/compilar_vin.sh, que dejan build/salida/imx296_mipi.ko y build/salida/vin/parche/vin_v4l2.ko.
# Salida: build/salida/imx296-vin_<versión>_arm64.deb
#
# El árbol del paquete está en paquete/; aquí se le agregan los módulos, los overlays compilados
# (dtc del paquete de headers, la misma versión 1.6.1 de la placa) y los scripts de scripts/placa/.
# Funciona en Linux y en Windows desde Git Bash.
set -euo pipefail
KVER=6.6.98-sun60iw2

if [ "${DENTRO_CONTENEDOR:-}" = 1 ]; then
	VERSION=$1
	P=/tmp/imx296-vin
	R=/repo
	for f in "$R/build/salida/imx296_mipi.ko" "$R/build/salida/vin/parche/vin_v4l2.ko"; do
		[ -e "$f" ] || { echo "Falta $f (ver el encabezado)"; exit 1; }
	done
	rm -rf "$P" && cp -r "$R/paquete" "$P"
	mkdir -p "$P/lib/modules/$KVER/updates" "$P/usr/share/imx296-vin/overlays" \
		"$P/usr/share/imx296-vin/scripts" "$P/usr/share/doc/imx296-vin"
	cp "$R/build/salida/imx296_mipi.ko" "$R/build/salida/vin/parche/vin_v4l2.ko" "$P/lib/modules/$KVER/updates/"
	for o in imx296-vin imx296-vin-cam2; do
		cp "$R/overlays/$o.dts" "$P/usr/share/imx296-vin/overlays/"
		"$KDIR/scripts/dtc/dtc" -q -@ -I dts -O dtb -o "$P/usr/share/imx296-vin/overlays/$o.dtbo" "$R/overlays/$o.dts"
	done
	for s in captura.sh prueba_modos.sh analizar_raw.py raw16_a_gris8.py estado_robustez.sh; do
		cp "$R/scripts/placa/$s" "$P/usr/share/imx296-vin/scripts/"
	done
	cp "$R/README.md" "$P/usr/share/doc/imx296-vin/README.md"
	cp "$R/driver/vin/README.md" "$P/usr/share/doc/imx296-vin/README.vin.md"

	# Un checkout en Windows puede dejar CRLF: los scripts no correrían en la placa.
	find "$P" -type f ! -name '*.ko' ! -name '*.dtbo' -exec sed -i 's/\r$//' {} +
	mv "$P/DEBIAN/control.in" "$P/DEBIAN/control"
	sed -i "s/@VERSION@/$VERSION/g; s/@KVER@/$KVER/g" "$P/DEBIAN/control" "$P/DEBIAN/preinst" \
		"$P/DEBIAN/postinst" "$P/DEBIAN/postrm"

	find "$P" -type d -exec chmod 755 {} +
	find "$P" -type f -exec chmod 644 {} +
	chmod 755 "$P"/DEBIAN/{preinst,postinst,prerm,postrm} "$P"/usr/bin/* "$P"/usr/sbin/* \
		"$P"/usr/lib/imx296-vin/* "$P"/usr/share/imx296-vin/scripts/*
	(cd "$P" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' | sort -z | xargs -0 md5sum > DEBIAN/md5sums)

	DEB="/salida/imx296-vin_${VERSION}_arm64.deb"
	dpkg-deb --root-owner-group --build "$P" "$DEB" > /dev/null
	echo "== $DEB"
	dpkg-deb --info "$DEB" | sed -n '/Package:/,$p'
	echo "== contenido"
	dpkg-deb --contents "$DEB" | awk '{print $1, $3, $6}'
	echo "== módulos"
	for m in imx296_mipi vin_v4l2; do
		f="$P/lib/modules/$KVER/updates/$m.ko"
		echo "$m: $(md5sum < "$f" | cut -c1-32) srcversion $(modinfo -F srcversion "$f") vermagic $(modinfo -F vermagic "$f")"
	done
	md5sum "$P"/usr/share/imx296-vin/overlays/*.dtbo | sed "s|$P||"
	exit 0
fi

RAIZ="$(cd "$(dirname "$0")/.." && pwd)"
IMG=opi-a733-kmod:6.6.98
VERSION=${1:-3.1}
[ -n "$(docker images -q "$IMG")" ] || docker build -t "$IMG" "$RAIZ/build"
ruta() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }
export MSYS_NO_PATHCONV=1
docker run --rm -e DENTRO_CONTENEDOR=1 \
	-v "$(ruta "$RAIZ"):/repo:ro" \
	-v "$(ruta "$RAIZ/build/salida"):/salida" \
	"$IMG" bash /repo/build/construir_deb.sh "$VERSION"
if ! command -v cygpath >/dev/null 2>&1; then
	docker run --rm -v "$RAIZ/build/salida:/salida" "$IMG" chown "$(id -u):$(id -g)" "/salida/imx296-vin_${VERSION}_arm64.deb"
fi
