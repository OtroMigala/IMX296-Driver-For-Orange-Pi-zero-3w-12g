#!/usr/bin/env bash
# Compila un módulo de sensor VIN desde Linux usando el contenedor Docker (equivalente a compilar.ps1).
# Uso:  build/compilar_linux.sh imx296_mipi            (o imx219 para validar el entorno)
#       build/compilar_linux.sh --rebuild imx296_mipi  (reconstruye la imagen Docker)
#       build/compilar_linux.sh --checkpatch           (revisa el estilo de driver/imx296_mipi.c)
set -euo pipefail
RAIZ="$(cd "$(dirname "$0")/.." && pwd)"
IMG=opi-a733-kmod:6.6.98

REBUILD=0
if [ "${1:-}" = --rebuild ]; then REBUILD=1; shift; fi
MOD="${1:?Falta el nombre del módulo (sin .c) o --checkpatch}"

if [ "$REBUILD" = 1 ] || [ -z "$(docker images -q "$IMG")" ]; then
  docker build -t "$IMG" "$RAIZ/build"
fi

if [ "$MOD" = --checkpatch ]; then
  exec docker run --rm -v "$RAIZ/driver:/driver:ro" "$IMG" \
    sh -c 'perl "$KDIR/scripts/checkpatch.pl" --no-tree --show-types -f /driver/imx296_mipi.c'
fi

[ -d "$RAIZ/fuentes/vin-8a9be72" ] || { echo "Falta fuentes/vin-8a9be72: correr scripts/descargar_fuentes.sh"; exit 1; }
mkdir -p "$RAIZ/build/salida"
docker run --rm \
  -v "$RAIZ/fuentes/vin-8a9be72:/fuentes:ro" \
  -v "$RAIZ/driver:/driver:ro" \
  -v "$RAIZ/build/salida:/salida" \
  "$IMG" compilar.sh "$MOD"
# El contenedor escribe como root: el .ko vuelve al usuario que compila.
docker run --rm -v "$RAIZ/build/salida:/salida" "$IMG" chown "$(id -u):$(id -g)" "/salida/$MOD.ko"
