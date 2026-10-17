#!/usr/bin/env bash
# Arma un MP4 (H.264) y fotos PNG a partir de cuadros gris de 8 bits (salida de raw16_a_gris8.py).
# Corre en el PC (Git Bash) con ffmpeg.
# Uso: gris8_a_mp4.sh <entrada.gray8> <salida_sin_extension> [fps] [ancho] [alto]
set -euo pipefail
# MSYS_NO_PATHCONV evita que Git Bash destroce el filtro de ffmpeg; por eso la ruta de
# entrada se convierte a formato Windows a mano (ffmpeg.exe no entiende /c/...).
export MSYS_NO_PATHCONV=1
ENT="$(cygpath -w "$1" 2>/dev/null || echo "$1")"; SAL="$2"; FPS="${3:-60}"; W="${4:-1456}"; H="${5:-1088}"
FONT="C\\:/Windows/Fonts/arial.ttf"
T1="Camara Raspberry Pi Global Shutter (Sony IMX296) en Orange Pi Zero 3W (Allwinner A733)"
T2="Driver VIN propio imx296_mipi - 1456x1088 RAW10 - 60 fps - Proyecto Ticanor"
CAJA="fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=8"
VF="drawtext=fontfile='$FONT':text='$T1':x=20:y=20:fontsize=30:$CAJA,"
VF+="drawtext=fontfile='$FONT':text='$T2':x=20:y=66:fontsize=26:$CAJA,"
VF+="drawtext=fontfile='$FONT':text='cuadro %{frame_num}':x=w-tw-20:y=h-th-20:fontsize=26:$CAJA"

# Video a velocidad real (60 fps)
ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format gray -video_size "${W}x${H}" \
  -framerate "$FPS" -i "$ENT" -vf "$VF" -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p \
  -movflags +faststart "${SAL}.mp4"

# Cámara lenta x4 (muestra los 60 cuadros por segundo uno a uno): el mismo H.264 con las marcas
# de tiempo estiradas, sin recodificar. Recodificar a 15 fps con el mismo CRF pesaba 11 veces más
# (30 MB, más que el límite de adjuntos del correo): x264 da más bits por cuadro a menos fps.
ffmpeg -hide_banner -loglevel error -y -itsscale 4 -i "${SAL}.mp4" -c copy -movflags +faststart \
  "${SAL}_camara_lenta_x4.mp4"

# Fotos: un cuadro cada medio segundo, sin rótulos
ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format gray -video_size "${W}x${H}" \
  -framerate "$FPS" -i "$ENT" -vf "select=not(mod(n\,$((FPS / 2))))" -fps_mode vfr "${SAL}_foto_%02d.png"

ls -la "${SAL}"*.mp4 "${SAL}"_foto_*.png
