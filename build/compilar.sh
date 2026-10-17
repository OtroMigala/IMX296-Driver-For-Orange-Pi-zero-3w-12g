#!/usr/bin/env bash
# Corre DENTRO del contenedor. Compila un módulo de sensor VIN fuera del árbol.
# Uso: compilar.sh <modulo>        ej: compilar.sh imx296_mipi   |   compilar.sh imx219
# Volúmenes esperados:
#   /fuentes  (solo lectura) = fuentes/vin-8a9be72  (bsp/drivers/vin y bsp/include del commit de la imagen)
#   /driver   (solo lectura) = driver/              (archivos .c propios; opcional)
#   /salida                  = build/salida/        (aquí queda el .ko)
set -euo pipefail
MOD="${1:?Falta el nombre del módulo (sin .c)}"

# 1) bsp/include al árbol de headers: el Makefile del kernel ya incluye -I$(srctree)/bsp/include
mkdir -p "$KDIR/bsp"
rm -rf "$KDIR/bsp/include"
cp -r /fuentes/bsp/include "$KDIR/bsp/include"

# 2) Copia de trabajo de VIN, para no ensuciar /fuentes
rm -rf /tmp/vin && cp -r /fuentes/bsp/drivers/vin /tmp/vin
S=/tmp/vin/modules/sensor
if [ -d /driver ] && ls /driver/*.c >/dev/null 2>&1; then cp /driver/*.c "$S/"; fi
[ -f "$S/$MOD.c" ] || { echo "No existe $MOD.c (ni en VIN ni en /driver)"; exit 1; }

# 3) Kbuild propio: tiene prioridad sobre el Makefile de VIN y compila solo este módulo.
#    Sin ccflags extra: el ccflags-y de bsp/drivers/vin/Makefile no se hereda a modules/sensor/,
#    así que el imx219.ko de la imagen se compiló sin -DDYNAMIC_DEBUG_MODULE.
cat > "$S/Kbuild" <<EOF
obj-m := $MOD.o
EOF

make -C "$KDIR" M="$S" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" KBUILD_MODPOST_WARN=0 modules

mkdir -p /salida
cp "$S/$MOD.ko" /salida/
echo
echo "== $MOD.ko"
modinfo /salida/$MOD.ko | grep -E "^(vermagic|depends|name|license)"
file /salida/$MOD.ko
