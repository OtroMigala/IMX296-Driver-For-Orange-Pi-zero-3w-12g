# Parches de VIN (`vin_v4l2.ko`)

VIN es el framework de cámaras de Allwinner. El módulo `vin_v4l2.ko` de la imagen de Orange Pi tiene dos usos de la cámara que terminan en *kernel panic* por puntero NULL. Ninguno es culpa del driver del sensor, y los dos aparecen cuando el conector CAM1 está vacío y la cámara está en CAM2. Estos parches los convierten en un error del ioctl, sin tocar el kernel: `vin_v4l2` es un módulo.

| Parche | Uso que antes daba *panic* | Ahora |
|---|---|---|
| `0001-vin-controles-sin-sensor-elegido.patch` | Leer o escribir controles antes de `VIDIOC_S_INPUT` (`v4l2-ctl --list-ctrls`): `vin_g_volatile_ctrl+0x78`, NULL en 0x1c | `ENODEV` en los controles que necesitan el sensor; los demás se listan igual |
| `0002-vin-stream-on-sin-formato.patch` | `VIDIOC_S_INPUT` en un proceso y `S_FMT` + `STREAMON` en otro: `sunxi_csi_subdev_s_stream+0x118`, NULL en 0x4 | `STREAMON` devuelve `EINVAL` y VIN queda listo para la próxima captura |

La captura normal no cambia: `S_INPUT`, `S_FMT` y `STREAMON` en la misma sesión, como hace `--set-input=0` en la misma invocación de `v4l2-ctl`.

## Por qué pasaba

- **Controles.** `vinc->sensor_sel` solo se asigna en `VIDIOC_S_INPUT`. Antes vale 0, es decir CAM1. Si ese conector no tiene sensor, `get_valid_sensor()` devuelve NULL, y además la tubería todavía no tiene el subdispositivo del sensor. `vin_g_volatile_ctrl()`, `vin_s_ctrl()`, `__vin_s_parm()` y `vin_s_parm_special()` usaban los dos punteros sin revisarlos.
- **STREAMON en otra sesión.** VIN abre la tubería y habilita los enlaces del grafo de medios en `S_INPUT`, y al cerrar el dispositivo deshace las dos cosas. `S_FMT` llega al CSI solo por enlaces habilitados. En otra sesión, `csi->csi_fmt` sigue en NULL y `__csi_set_fmt_hw()` lo usaba. Tampoco había una salida limpia: `vin_close()` tiene una salida temprana para cuando el sensor no se usó en la sesión, y ahí deja `VIN_STREAM` activo. Con eso, todo `S_FMT` y `STREAMON` posterior falla hasta recargar el módulo.

## Qué cambia

**0001:**
- `vin_g_volatile_ctrl()` y `vin_s_ctrl()` devuelven `-ENODEV` sin sensor válido o sin subdispositivo de sensor.
- En `vin_s_ctrl()` la comprobación va después de marcar los controles como volátiles y antes del código de VFLIP/HFLIP, que escribe registros del DMA.
- `__vin_s_parm()` y `vin_s_parm_special()` hacen lo mismo con `inst`. También cubre el caso en que un `S_INPUT` fallido cambió `sensor_sel` y la tubería conserva el sensor de antes.

**0002:**
- `vidioc_streamon()` rechaza con `-EINVAL`, antes de tocar nada, si en esta sesión no se abrió la tubería. Es la misma condición que usa `vin_close()`: tiene que haber subdispositivo de sensor y con `use_count > 0`. Es el arreglo principal.
- `sunxi_csi_subdev_s_stream()` es una defensa de respaldo. Si no hay `csi_fmt`, devuelve `-EINVAL` antes de encender pines, reloj y *parser*. Tiene que ser `-EINVAL` porque `__vin_pipeline_s_stream()` ignora `-ENODEV`.
- Camino de error de la tubería. En el kernel 6.6, `call_s_stream()` lleva la cuenta del estado de cada subdispositivo y, si se le pide apagar uno que no arrancó, da un WARNING y no hace nada. Por eso hay dos cambios:
  - `__vin_pipeline_s_stream()` apaga solo los subdispositivos anteriores al que falló y conserva el error. Antes lo pisaba con el resultado del apagado y `STREAMON` respondía "éxito" sin transmitir nada.
  - `__vin_subdev_set_stream()` descuenta su contador cuando un arranque falla. Si no, el próximo arranque se salta ese subdispositivo y no llegan cuadros.

## Probado

Pruebas del 2026-10-20, Orange Pi Zero 3W con la imagen 1.0.2 y la cámara GS (IMX296LL) en CAM2:

| Prueba | Original | Con los parches |
|---|---|---|
| `--list-ctrls` sin `S_INPUT` | *panic* | rc 0, `error 19` (ENODEV) en los controles volátiles |
| `S_INPUT` y `STREAMON` en procesos distintos | *panic* | `STREAMON` → `EINVAL`, sin WARNING |
| `S_INPUT` + `STREAMON` sin `S_FMT` (`csi_fmt` NULL, recién arrancada) | *panic* según el código (mismo camino; no se probó) | `EINVAL` por la defensa del CSI, sin WARNING |
| Captura normal después de cada rechazo, 3 vueltas | — | 120 cuadros sin saltos a 60 fps |
| 3600 cuadros sin escribir | 36 000 sin saltos (etapa 1) | sin saltos, 60,001 fps |
| 120 cuadros a `/dev/shm` con 4 búferes, 5 veces | 4 de 5 con un salto | 5 de 5 con un salto |
| 180 cuadros a `/dev/shm` con 8 búferes | sin saltos | sin saltos |

Con solo 4 búferes, escribir cada cuadro de 3 MB a veces se atrasa y se pierde uno. Pasa igual con el original, así que conviene usar 8 o más.

## Compilar e instalar

`build/compilar_vin.sh` compila `vin_io.ko` y `vin_v4l2.ko` desde `fuentes/vin-8a9be72` y aplica estos parches en orden. Con `--original` compila VIN sin modificar. Con `--comparar <carpeta>` lo contrasta con los módulos de la imagen: sin parches, todo sale idéntico (`srcversion`, bytes de código y relocalizaciones).

En la placa, sin pisar el original:

```bash
sudo install -D -m 644 vin_v4l2.ko /lib/modules/$(uname -r)/updates/vin_v4l2.ko
sudo depmod -a        # updates/ tiene prioridad (/etc/depmod.d/ubuntu.conf)
modinfo -n vin_v4l2   # .../updates/vin_v4l2.ko
```

`vin_io.ko` no cambia, se usa el de la imagen. Para volver al original: `sudo rm /lib/modules/$(uname -r)/updates/vin_v4l2.ko && sudo depmod -a`. VIN no se carga al arrancar, así que un módulo con problemas no impide arrancar.

## Otros bugs vistos, sin corregir

- `__vin_s_input()` sale sin soltar `graph_mutex` si falla `vin_pipeline_call(open)`.
- `__vin_s_input()` cambia `sensor_sel` antes de validar el sensor. Si falla, queda apuntando a un módulo sin sensor (0001 evita que eso termine en *panic*).
- `vidioc_streamon()` marca `VIN_STREAM` aunque la tubería falle. Con 0002 eso solo puede pasar en una sesión con `S_INPUT`, y ahí `vin_close()` o `STREAMOFF` lo limpian.
