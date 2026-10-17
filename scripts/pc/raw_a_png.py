"""Analiza una captura RAW10 desempaquetada (16 bits por píxel, little-endian) y genera PNG en gris.

Uso: py -3 -I raw_a_png.py <archivo.raw> <ancho> <alto> <salida_prefijo> [--rot180]
  --rot180: gira la imagen 180° (el sensor de la cámara GS va montado invertido).
Sin dependencias externas (array + zlib).
"""
import sys, zlib, struct
from array import array

args = [a for a in sys.argv[1:] if not a.startswith("--")]
ROT180 = "--rot180" in sys.argv
ruta, W, H, pref = args[0], int(args[1]), int(args[2]), args[3]
tam = W * H * 2
datos = open(ruta, "rb").read()
n = len(datos) // tam
print(f"archivo: {len(datos)} bytes -> {n} cuadros de {W}x{H} (16 bits/píxel)")


def png_gris(nombre, px8, w, h):
    filas = b"".join(b"\x00" + bytes(px8[y * w:(y + 1) * w]) for y in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    with open(nombre, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(filas, 6)))
        f.write(chunk(b"IEND", b""))


for i in sorted({0, n - 1}):
    px = array("H")
    px.frombytes(datos[i * tam:(i + 1) * tam])
    if sys.byteorder != "little":
        px.byteswap()
    if ROT180:
        px.reverse()  # invertir todo el búfer = espejo horizontal + vertical = giro de 180°
    mn, mx = min(px), max(px)
    media = sum(px) / len(px)
    bits = 0
    for v in px[::97]:
        bits |= v
    hist = [0] * 16
    for v in px[::13]:
        hist[v >> 12] += 1
    print(f"cuadro {i}: min={mn} max={mx} media={media:.1f}  bits usados=0x{bits:04x}")
    print(f"   histograma (16 tramos de 4096, muestreado): {hist}")
    # Alineación: RAW10 a la derecha (<=1023) o a la izquierda (múltiplos de 64)
    desplaz = 2 if mx <= 1023 else 8
    print(f"   -> alineación {'derecha (0..1023)' if desplaz == 2 else 'izquierda (16 bits)'}; >> {desplaz} para 8 bits")
    px8 = bytes(min(255, v >> desplaz) for v in px)
    png_gris(f"{pref}_cuadro{i}.png", px8, W, H)
    # Versión con contraste estirado (percentiles 1 y 99) para ver detalle
    muestra = sorted(px[::31])
    lo = muestra[len(muestra) // 100]
    hi = muestra[len(muestra) * 99 // 100]
    rango = max(1, hi - lo)
    px8e = bytes(0 if v <= lo else 255 if v >= hi else (v - lo) * 255 // rango for v in px)
    png_gris(f"{pref}_cuadro{i}_contraste.png", px8e, W, H)
    print(f"   PNG: {pref}_cuadro{i}.png y _contraste.png (estirado {lo}..{hi})")
