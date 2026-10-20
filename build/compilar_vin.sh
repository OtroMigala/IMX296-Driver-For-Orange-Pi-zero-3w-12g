#!/usr/bin/env bash
# Compila vin_io.ko y vin_v4l2.ko (el framework de cámaras VIN de Allwinner) desde
# fuentes/vin-8a9be72, con o sin los parches de driver/vin/, en el contenedor Docker de build/.
#
# Uso:  build/compilar_vin.sh                 VIN con los parches driver/vin/*.patch, en orden
#       build/compilar_vin.sh --original      VIN sin modificar
#       build/compilar_vin.sh ... --comparar <carpeta con vin_io.ko y vin_v4l2.ko de la imagen>
# Salida: build/salida/vin/parche/ o build/salida/vin/original/
#
# --comparar sirve para validar que fuentes/vin-8a9be72 es el código con que Orange Pi compiló
# la imagen: compara srcversion (hash que modpost calcula del contenido de los fuentes), los
# símbolos importados y definidos, los bytes de las secciones de código y datos y las
# relocalizaciones. Resultado con los módulos de la imagen 1.0.2: todo idéntico.
# Funciona en Linux y en Windows desde Git Bash.
set -euo pipefail

if [ "${DENTRO_CONTENEDOR:-}" = 1 ]; then
	MODO=$1
	# bsp/include al árbol de headers: el Makefile del kernel ya incluye -I$(srctree)/bsp/include
	mkdir -p "$KDIR/bsp" && rm -rf "$KDIR/bsp/include" && cp -r /fuentes/bsp/include "$KDIR/bsp/include"
	# Copia de trabajo con la misma ruta que en el árbol del kernel, para que los parches
	# (relativos a la raíz del kernel) se apliquen igual aquí y en orangepi-build.
	rm -rf /tmp/k && mkdir -p /tmp/k/bsp/drivers && cp -r /fuentes/bsp/drivers/vin /tmp/k/bsp/drivers/vin
	# Un checkout en Windows (core.autocrlf=true) deja las fuentes con CRLF: se pasan a LF, como en
	# el árbol de Orange Pi, para que los parches (LF) se apliquen y srcversion coincida.
	find /tmp/k -type f \( -name '*.[ch]' -o -name Makefile -o -name Kconfig \) -exec sed -i 's/\r$//' {} +
	if [ "$MODO" = parche ]; then
		for p in /parches/*.patch; do
			[ -e "$p" ] || { echo "No hay parches en driver/vin/"; exit 1; }
			echo "== aplicando $(basename "$p")"
			patch -d /tmp/k -p1 -F0 --no-backup-if-mismatch < "$p"
		done
	fi
	make -C "$KDIR" M=/tmp/k/bsp/drivers/vin ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
		-j"$(nproc)" modules > /tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }
	grep -iE "warning|error" /tmp/make.log | grep -v "^make" || echo "(sin advertencias)"
	mkdir -p "/salida/vin/$MODO"
	for m in vin_io vin_v4l2; do
		cp "/tmp/k/bsp/drivers/vin/$m.ko" "/salida/vin/$MODO/"
		echo "== $m.ko: $(modinfo -F srcversion "/salida/vin/$MODO/$m.ko") $(modinfo -F vermagic "/salida/vin/$MODO/$m.ko")"
	done
	[ -d /orig ] || exit 0
	NM=${CROSS_COMPILE}nm SIZE=${CROSS_COMPILE}size
	for m in vin_io vin_v4l2; do
		a=/orig/$m.ko b=/salida/vin/$MODO/$m.ko
		echo
		echo "===== $m: imagen (izquierda) vs. compilado (derecha)"
		echo "srcversion: $(modinfo -F srcversion "$a") / $(modinfo -F srcversion "$b")"
		echo "depends:    $(modinfo -F depends "$a") / $(modinfo -F depends "$b")"
		diff <($NM -u "$a" | awk '{print $2}' | sort) <($NM -u "$b" | awk '{print $2}' | sort) \
			> /tmp/imp.diff && echo "símbolos importados: iguales ($($NM -u "$b" | wc -l))" \
			|| { echo "símbolos importados: DISTINTOS"; cat /tmp/imp.diff; }
		diff <($NM --defined-only "$a" | awk '$2 ~ /[TDRB]/ {print $3}' | sort) \
			<($NM --defined-only "$b" | awk '$2 ~ /[TDRB]/ {print $3}' | sort) \
			> /tmp/def.diff && echo "símbolos globales definidos: iguales" \
			|| { echo "símbolos globales definidos: DISTINTOS"; head -20 /tmp/def.diff; }
		# En aarch64 las relocalizaciones van aparte (RELA): si el código es el mismo, los bytes de
		# las secciones y la tabla de relocalizaciones son idénticos. Se excluye la información de
		# depuración, que la imagen guarda en un archivo aparte (.gnu_debuglink).
		for s in .text .rodata .data .init.text .exit.text; do
			[ "$(${CROSS_COMPILE}objcopy -O binary -j $s "$a" /dev/stdout | md5sum)" = \
			  "$(${CROSS_COMPILE}objcopy -O binary -j $s "$b" /dev/stdout | md5sum)" ] \
				&& echo "bytes de $s: iguales ($($SIZE -A "$b" | awk -v s=$s '$1 == s {print $2}') B)" \
				|| echo "bytes de $s: DISTINTOS"
		done
		rel() { ${CROSS_COMPILE}readelf -rW "$1" 2>/dev/null |
			awk '/Relocation section/ {sec = $3} /^[0-9a-f]+ / {print sec, $1, $3, $5, $6, $7}' |
			grep -vE 'debug|__dyndbg|\.modinfo|\.note'; }
		diff <(rel "$a") <(rel "$b") > /tmp/rel.diff && echo "relocalizaciones: iguales ($(rel "$b" | wc -l))" \
			|| { echo "relocalizaciones: DISTINTAS"; head -10 /tmp/rel.diff; }
	done
	exit 0
fi

RAIZ="$(cd "$(dirname "$0")/.." && pwd)"
IMG=opi-a733-kmod:6.6.98
MODO=parche
ORIG=
while [ $# -gt 0 ]; do
	case "$1" in
		--original) MODO=original ;;
		--comparar) ORIG=${2:?Falta la carpeta para --comparar}; shift ;;
		*) echo "Opción desconocida: $1 (ver el encabezado)"; exit 1 ;;
	esac
	shift
done
[ -d "$RAIZ/fuentes/vin-8a9be72" ] || { echo "Falta fuentes/vin-8a9be72: correr scripts/descargar_fuentes.sh"; exit 1; }
[ -n "$(docker images -q "$IMG")" ] || docker build -t "$IMG" "$RAIZ/build"
docker run --rm "$IMG" sh -c 'command -v patch >/dev/null' \
	|| { echo "La imagen $IMG no trae patch: reconstruirla con docker build -t $IMG build/"; exit 1; }

# Git Bash convierte las rutas de los argumentos (y rompe "-v origen:destino"): se desactiva y
# se pasan rutas de Windows.
ruta() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }
export MSYS_NO_PATHCONV=1
mkdir -p "$RAIZ/build/salida/vin" "$RAIZ/driver/vin"
VOL=(-v "$(ruta "$RAIZ/fuentes/vin-8a9be72"):/fuentes:ro"
	-v "$(ruta "$RAIZ/driver/vin"):/parches:ro"
	-v "$(ruta "$RAIZ/build/salida"):/salida"
	-v "$(ruta "$RAIZ/build/compilar_vin.sh"):/compilar_vin.sh:ro")
[ -n "$ORIG" ] && VOL+=(-v "$(ruta "$(cd "$ORIG" && pwd)"):/orig:ro")
docker run --rm -e DENTRO_CONTENEDOR=1 "${VOL[@]}" "$IMG" bash /compilar_vin.sh "$MODO"
# En Linux el contenedor escribe como root: los .ko vuelven al usuario que compila.
if ! command -v cygpath >/dev/null 2>&1; then
	docker run --rm -v "$RAIZ/build/salida:/salida" "$IMG" chown -R "$(id -u):$(id -g)" /salida/vin
fi
