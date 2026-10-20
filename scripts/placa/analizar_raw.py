#!/usr/bin/env python3
"""Estadísticas por cuadro de un archivo RAW de VIN (16 bits little-endian por píxel).

Uso: python3 analizar_raw.py <archivo.raw> <ancho> <alto> [--valores] [--cada N] [--tam-cuadro B]
  Por cuadro: media, mínimo y máximo (0..1023).
  --valores:      además, los valores distintos del primer cuadro y cuántos píxeles tiene cada
                  uno (para el patrón de prueba, que debe dar solo los valores esperados).
  --cada N:       muestra uno de cada N cuadros (por defecto, todos).
  --tam-cuadro B: bytes de cada cuadro en el archivo, si VIN agrega relleno al final (Size
                  Image de --get-fmt-video; en 728x544 son 800768 en vez de 792064).
"""
import argparse

import numpy as np


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("archivo")
    p.add_argument("ancho", type=int)
    p.add_argument("alto", type=int)
    p.add_argument("--valores", action="store_true")
    p.add_argument("--cada", type=int, default=1)
    p.add_argument("--tam-cuadro", type=int, default=0)
    a = p.parse_args()

    datos = np.fromfile(a.archivo, dtype="<u2")
    por_cuadro = a.ancho * a.alto
    paso = a.tam_cuadro // 2 if a.tam_cuadro else por_cuadro
    n = datos.size // paso
    if n == 0 or paso < por_cuadro:
        p.exit(1, f"{a.archivo}: menos de un cuadro de {a.ancho}x{a.alto}\n")
    cuadros = datos[: n * paso].reshape(n, paso)[:, :por_cuadro].reshape(n, a.alto, a.ancho)
    print(f"{n} cuadros de {a.ancho}x{a.alto}")
    for i in range(0, n, a.cada):
        c = cuadros[i]
        print(f"cuadro {i:4d}: media {c.mean():7.2f}  min {c.min():4d}  max {c.max():4d}")

    if a.valores:
        valores, cuentas = np.unique(cuadros[0], return_counts=True)
        print(f"valores distintos en el cuadro 0: {valores.size}")
        for v, k in zip(valores[:20], cuentas[:20]):
            print(f"  {v:4d} (0x{v:03x}): {k} píxeles")


if __name__ == "__main__":
    main()
