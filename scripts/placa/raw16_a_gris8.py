#!/usr/bin/env python3
"""Convierte cuadros RAW10 desempaquetados (16 bits LE, 0..1023) a gris de 8 bits para video.

Corre en la placa (numpy). Niveles automáticos: negro = percentil 0,5 y blanco = percentil
99,5 de una muestra de cuadros, más una curva gamma suave para que la escena se vea natural.

Uso: python3 raw16_a_gris8.py <entrada.raw> <salida.gray8> [ancho] [alto] [gamma]
"""
import sys
import numpy as np

ent, sal = sys.argv[1], sys.argv[2]
W = int(sys.argv[3]) if len(sys.argv) > 3 else 1456
H = int(sys.argv[4]) if len(sys.argv) > 4 else 1088
GAMMA = float(sys.argv[5]) if len(sys.argv) > 5 else 0.7

cuadros = np.memmap(ent, dtype="<u2", mode="r")
n = cuadros.size // (W * H)
cuadros = cuadros[: n * W * H].reshape(n, H, W)
muestra = cuadros[:: max(1, n // 8), ::4, ::4].astype(np.float32)
negro, blanco = np.percentile(muestra, [0.5, 99.5])
blanco = max(blanco, negro + 1)
print(f"{n} cuadros {W}x{H}; niveles: negro={negro:.0f} blanco={blanco:.0f} gamma={GAMMA}")

lut = np.clip((np.arange(1024, dtype=np.float32) - negro) / (blanco - negro), 0, 1) ** GAMMA
lut = (lut * 255 + 0.5).astype(np.uint8)
with open(sal, "wb") as f:
    for i in range(n):
        f.write(lut[np.minimum(cuadros[i], 1023)].tobytes())
print(f"escrito {sal}: {n * W * H} bytes")
