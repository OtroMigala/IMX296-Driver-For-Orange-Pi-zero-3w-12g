#!/usr/bin/env bash
# Fase 0 (en el PC): descarga solo lo necesario para el port.
# Uso: ./01_fetch_sources.sh [rama]
#   rama por defecto: orange-pi-6.6-sun60iw2  (usar orange-pi-5.15-sun60iw2 si uname -r en la placa es 5.15.x)
set -euo pipefail
# En Git Bash (Windows) evita que MSYS convierta "/bsp/..." en "C:/Program Files/Git/bsp/..."
export MSYS_NO_PATHCONV=1

RAMA="${1:-orange-pi-6.6-sun60iw2}"
DEST="$(dirname "$0")/../fuentes"
mkdir -p "$DEST" && cd "$DEST"

echo "== Kernel de Orange Pi, rama $RAMA (clon parcial)"
if [ ! -d kernel ]; then
  # core.protectNTFS=false: en Windows git rechaza el árbol porque trae un drivers/.../aux.c
  # (nombre reservado en NTFS). Ese archivo queda fuera del sparse-checkout y no se escribe.
  git clone --config core.protectNTFS=false --depth 1 --filter=blob:none --sparse -b "$RAMA" \
    https://github.com/orangepi-xunlong/linux-orangepi.git kernel
fi
cd kernel
git sparse-checkout set --no-cone \
  /bsp/drivers/vin/ \
  /drivers/media/i2c/imx296.c \
  /drivers/media/i2c/imx290.c \
  /Documentation/devicetree/bindings/media/i2c/sony,imx296.yaml \
  /arch/arm64/boot/dts/allwinner/sun60i-a733-orangepi-zero3w.dts \
  '/arch/arm64/boot/dts/allwinner/sun60i-a733*.dtsi' \
  /arch/arm64/boot/dts/allwinner/overlay/
cd ..

echo "== Driver y overlay IMX296 de Raspberry Pi (con disparo externo)"
mkdir -p rpi
curl -fsSL -o rpi/imx296.c \
  https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.6.y/drivers/media/i2c/imx296.c
curl -fsSL -o rpi/imx296-overlay.dts \
  https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.6.y/arch/arm/boot/dts/overlays/imx296-overlay.dts

echo "== Configuración del kernel A733 y familia sun60iw2 (orangepi-build)"
mkdir -p orangepi-build
curl -fsSL -o orangepi-build/linux-sun60iw2-current-a733.config \
  https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/next/external/config/kernel/linux-sun60iw2-current-a733.config
curl -fsSL -o orangepi-build/sun60iw2.conf \
  https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/next/external/config/sources/families/sun60iw2.conf

echo "== VIN del commit 8a9be72 (bsp/drivers/vin y bsp/include), para build/compilar.sh"
# La imagen 1.0.2 no registra el commit de su kernel; 8a9be72 es el último de la rama antes
# de esa compilación (2026-04-07). Los .h de VIN cambiaron después, por eso no se usa HEAD.
VIN_COMMIT=8a9be72c9006a87f786736b3aa4e2dfd971c1429
if [ "$RAMA" = orange-pi-6.6-sun60iw2 ] && [ ! -d vin-8a9be72 ]; then
  git -C kernel fetch -q --shallow-since=2026-02-01 origin "$RAMA"
  mkdir -p vin-8a9be72
  git -C kernel archive "$VIN_COMMIT" bsp/drivers/vin bsp/include | tar -x -C vin-8a9be72
fi

echo
echo "Listo. Archivos clave:"
echo "  fuentes/kernel/bsp/drivers/vin/modules/sensor/imx290_mipi.c   (plantilla)"
echo "  fuentes/kernel/bsp/drivers/vin/modules/sensor/imx219.c        (comparación, 2 cámaras)"
echo "  fuentes/kernel/drivers/media/i2c/imx296.c                     (registros, kernel principal)"
echo "  fuentes/rpi/imx296.c                                          (registros + disparo externo)"
echo
echo "  fuentes/vin-8a9be72/                                          (VIN para compilar el módulo)"
echo
echo "Para compilar: copiar a build/ el paquete de headers de la imagen"
echo "  (/opt/linux-headers-current-sun60iw2_1.0.0_arm64.deb en la placa) y correr build/compilar.ps1."
