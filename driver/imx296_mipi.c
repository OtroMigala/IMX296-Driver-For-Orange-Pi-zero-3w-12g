// SPDX-License-Identifier: GPL-2.0
/*
 * Driver VIN (Allwinner) para el sensor Sony IMX296 de la Raspberry Pi
 * Global Shutter Camera, en la Orange Pi Zero 3W (A733, sun60iw2).
 *
 * RAW10, 1 carril MIPI, reloj de 54 MHz (oscilador propio de la cámara). Modos:
 * 1456x1088 a 60 cuadros/s, 1456x544 (recorte vertical centrado) y 728x544
 * (binning 2x2) a 100 cuadros/s. Los fps se bajan con el control VBLANK.
 *
 * Derivado de:
 *   - bsp/drivers/vin/modules/sensor/imx219.c (Allwinner, GPL-2.0-or-later):
 *     enlace con VIN, compatibilidad con el kernel 6.6 y las dos instancias.
 *   - bsp/drivers/vin/modules/sensor/imx290_mipi.c (Allwinner, GPL-2.0-or-later):
 *     convenciones Sony (standby en 0x3000, exposición por SHS).
 *   - drivers/media/i2c/imx296.c (Laurent Pinchart, GPL-2.0) y su versión de
 *     Raspberry Pi (rama rpi-6.6.y): todos los valores de registros, los controles
 *     (exposición, ganancia, VBLANK, espejo, patrón de prueba), el recorte, el
 *     binning, la temperatura y el modo de disparo externo.
 *
 * Instalación, uso y limitaciones conocidas de VIN: README.md.
 *
 * Versiones:
 *   v1 (2026-10-17): primera captura, 10 cuadros 1456x1088 a 60 fps en CAM2.
 *   v2 (2026-10-17): parámetros rotation (180 por defecto, como Raspberry Pi),
 *       exposure_us y gain_db10; la exposición y la ganancia vigentes se aplican
 *       después de la tabla al arrancar la captura en lugar de quedar fijas en ella.
 *   v2.1 (2026-10-17): limpieza sin cambios de comportamiento: parámetros con
 *       rango validado, mensajes del log en inglés y con niveles coherentes,
 *       códigos de error uniformes, estilo según checkpatch.pl. Solo kernel 6.6
 *       (se quitan las ramas LINUX_VERSION_CODE heredadas de imx219.c) y salida
 *       limpia en probe/remove si el nombre del cliente I2C no es de este driver.
 *   v3 (2026-10-20): controles V4L2 en el subdispositivo del sensor
 *       (/dev/v4l-subdevN), aplicados en vivo: exposición (líneas) y ganancia
 *       analógica (0,1 dB) en un mismo bloque REGHOLD, VBLANK, espejo H/V, patrón de
 *       prueba y temperatura del sensor. Modos 1456x544 (recorte) y 728x544
 *       (binning) a 100 fps. Los parámetros rotation, exposure_us y gain_db10 pasan
 *       a ser solo valores iniciales (al cargar el módulo). Corrige la exposición
 *       máxima: VMAX - 4 líneas, porque con SHS1 < 4 el sensor entrega cuadros negros
 *       (en v2.1, una exposure_us más larga que el cuadro daba cuadros negros).
 *
 * Uso con VIN (ver scripts/placa/captura.sh): cargar vin_io, este módulo y luego
 * vin_v4l2, y hacer VIDIOC_S_INPUT en la misma sesión que captura.
 */
#include "../../utility/vin_log.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <linux/clk.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mediabus.h>
#include <linux/io.h>
#include "camera.h"
#include "sensor_helper.h"

MODULE_AUTHOR("Proyecto Ticanor");
MODULE_DESCRIPTION("VIN driver for the Sony IMX296 (Raspberry Pi Global Shutter Camera)");
MODULE_LICENSE("GPL");

/*
 * Parámetros del módulo con rango validado: un valor fuera de rango se rechaza con
 * -EINVAL (en insmod o al escribir en /sys/module/imx296_mipi/parameters/) en lugar
 * de interpretarse en silencio. step: los valores válidos son min, min + step, ...
 */
struct imx296_param {
	int val;
	int min;
	int max;
	int step;
};

static int imx296_param_set(const char *buf, const struct kernel_param *kp)
{
	struct imx296_param *p = kp->arg;
	int v, ret;

	ret = kstrtoint(buf, 0, &v);
	if (ret)
		return ret;
	if (v < p->min || v > p->max || (v - p->min) % p->step)
		return -EINVAL;

	p->val = v;
	return 0;
}

static int imx296_param_get(char *buf, const struct kernel_param *kp)
{
	const struct imx296_param *p = kp->arg;

	return sysfs_emit(buf, "%d\n", p->val);
}

static const struct kernel_param_ops imx296_param_ops = {
	.set = imx296_param_set,
	.get = imx296_param_get,
};

/* 0 = libre (normal), 1 = disparo externo por XTR. Igual que el driver de Raspberry Pi. */
static struct imx296_param trigger_mode = { .val = 0, .min = 0, .max = 1, .step = 1 };
module_param_cb(trigger_mode, &imx296_param_ops, &trigger_mode, 0644);
MODULE_PARM_DESC(trigger_mode, "Set trigger mode: 0=default, 1=XTRIG");

/*
 * Valores iniciales de los controles del sensor; solo se leen al cargar el módulo
 * (0444). Después, la orientación, la exposición y la ganancia se cambian con los
 * controles horizontal_flip/vertical_flip, exposure y analogue_gain de /dev/v4l-subdevN.
 *
 * Orientación: el sensor va montado invertido en la cámara GS (imx296-overlay.dts de
 * Raspberry Pi declara rotation = <180>). 180 = espejo H+V en el sensor (CTRL0E = 0x03),
 * 0 = sin espejo. En la variante color (LQ) el espejo cambia el orden Bayer (RGGB -> BGGR);
 * las cámaras del proyecto son mono (LL), así que no afecta.
 */
static struct imx296_param rotation = { .val = 180, .min = 0, .max = 180, .step = 180 };
module_param_cb(rotation, &imx296_param_ops, &rotation, 0444);
MODULE_PARM_DESC(rotation, "Initial rotation applied in the sensor: 180 (default, as Raspberry Pi) or 0");

/*
 * Con isp_used = 1, VIN manda los controles de exposición y ganancia del nodo de video al
 * ISP (no al sensor): por eso se ajustan en el subdispositivo del sensor. Una exposición más
 * larga que el cuadro se recorta al aplicarse (VMAX - 4 líneas, ~16600 us a 60 fps).
 */
static struct imx296_param exposure_us = { .val = 10000, .min = 1, .max = 1000000, .step = 1 };
module_param_cb(exposure_us, &imx296_param_ops, &exposure_us, 0444);
MODULE_PARM_DESC(exposure_us, "Initial exposure in microseconds, 1..1000000 (default 10000; max ~16600 at 60 fps)");

static struct imx296_param gain_db10 = { .val = 0, .min = 0, .max = 480, .step = 1 };
module_param_cb(gain_db10, &imx296_param_ops, &gain_db10, 0444);
MODULE_PARM_DESC(gain_db10, "Initial analogue gain in 0.1 dB steps, 0..480 (default 0)");

#define V4L2_IDENT_SENSOR	0x0296

/* 0x1a en 7 bits; VIN la guarda en 8 bits y hace >> 1 (vin.c, __vin_register_module) */
#define I2C_ADDR		0x34
#define SENSOR_NUM		0x2
#define SENSOR_NAME		"imx296_mipi"
#define SENSOR_NAME_2		"imx296_mipi_2"

/*
 * Registros (nombres del driver del kernel principal).
 * Multibyte: byte bajo en la dirección menor.
 */
#define IMX296_CTRL00		0x3000	/* bit 0: STANDBY */
#define IMX296_CTRL08		0x3008	/* bit 0: REGHOLD */
#define IMX296_CTRL0A		0x300a	/* bit 0: XMSTA (1 = transmisión detenida) */
#define IMX296_CTRL0B		0x300b	/* bit 0: TRIGEN */
#define IMX296_CTRL0D		0x300d	/* modo de lectura: completo o binning */
#define IMX296_CTRL0E		0x300e	/* bit 0: VREVERSE, bit 1: HREVERSE */
#define IMX296_VMAX		0x3010	/* 24 bits, líneas por cuadro */
#define IMX296_TMDCTRL		0x301d	/* bit 0: LATCH */
#define IMX296_TMDOUT		0x301e	/* 16 bits, 10 útiles */
#define IMX296_BLKLEVELAUTO	0x3022
#define IMX296_SHS1		0x308d	/* 24 bits */
#define IMX296_LOWLAGTRG	0x30ae	/* bit 0: FAST */
#define IMX296_SENSOR_INFO	0x3148	/* 16 bits, solo legible fuera de standby */
#define IMX296_GAIN		0x3204	/* 16 bits, 0..480 = 0..48 dB en pasos de 0,1 dB */
#define IMX296_PGCTRL		0x3238	/* generador de patrones de prueba */
#define IMX296_PGHPOS		0x3239	/* 16 bits */
#define IMX296_PGVPOS		0x323c	/* 16 bits */
#define IMX296_PGHPSTEP		0x323e
#define IMX296_PGVPSTEP		0x323f
#define IMX296_PGHPNUM		0x3240
#define IMX296_PGVPNUM		0x3241
#define IMX296_PGDATA1		0x3244	/* 16 bits */
#define IMX296_PGDATA2		0x3246	/* 16 bits */
#define IMX296_PGHGSTEP		0x3249
#define IMX296_BLKLEVEL		0x3254	/* 16 bits */

#define IMX296_SENSOR_INFO_MONO	0x8000
#define IMX296_TMDOUT_MASK	0x3ff
#define IMX296_BLKLEVELAUTO_ON	0x01
#define IMX296_BLKLEVELAUTO_OFF	0xf0
#define IMX296_PGCTRL_REGEN	BIT(0)
#define IMX296_PGCTRL_CLKEN	BIT(2)
#define IMX296_PGCTRL_MODE(n)	((n) << 3)
#define IMX296_BLKLEVEL_DEF	60

#define IMX296_WIDTH		1456
#define IMX296_HEIGHT		1088
/*
 * HMAX = 1100 ciclos de un reloj interno de 74,25 MHz = 14,815 us por línea, en todos
 * los modos (la línea no se acorta con el recorte horizontal). En píxeles a la tasa
 * nominal de 118,8 MHz (1188 Mbps / 10 bits): 1100 * 118,8 / 74,25 = 1760.
 */
#define IMX296_HTS		1760
#define IMX296_PCLK		118800000
#define IMX296_MIPI_BPS		1188000000
/*
 * Cuadro = VMAX líneas = alto + VBLANK. Además de la imagen, el sensor saca 1 línea de inicio
 * de cuadro, 2 de datos embebidos, 6 nulas, 4 de negro óptico, 1 de fin y al menos 16 de
 * borrado: VBLANK >= 30. 118,8 MHz / (1760 * VMAX) = 67500 / VMAX cuadros/s.
 */
#define IMX296_VTS_60FPS	1125
#define IMX296_VTS_100FPS	675
#define IMX296_VMAX_MAX		0xfffff	/* VMAX es de 20 bits efectivos (kernel principal) */
/* Tiempo de línea: 1100 / 74,25 MHz = 14 814,8 ns */
#define IMX296_LINE_NS		14815
#define IMX296_GAIN_REG_MAX	480
/*
 * SHS1 (inicio de la exposición, contado desde el comienzo del cuadro) no puede bajar de 4:
 * con 1, 2 o 3 el sensor entrega cuadros negros (medido a 60 y a 100 fps; el kernel principal
 * y el de Raspberry Pi permiten 0). Exposición máxima = VMAX - 4 líneas.
 */
#define IMX296_SHS1_MIN		4
#define IMX296_EXP_MAX		(IMX296_VMAX_MAX - IMX296_SHS1_MIN)

/* Control propio: temperatura del sensor en milésimas de grado. */
#define IMX296_CID_TEMPERATURE	(V4L2_CID_USER_BASE + 0x2960)

/*
 * Ganancia lineal en Q4 (16 = 1x, la unidad de VIN) para cada valor del registro GAIN:
 * imx296_gain_q4[i] = round(16 * 10^(i / 200)), i = 0..480 (0,1 dB por paso).
 */
static const u16 imx296_gain_q4[IMX296_GAIN_REG_MAX + 1] = {
	  16,  16,  16,  17,  17,  17,  17,  17,  18,  18,  18,  18,
	  18,  19,  19,  19,  19,  19,  20,  20,  20,  20,  21,  21,
	  21,  21,  22,  22,  22,  22,  23,  23,  23,  23,  24,  24,
	  24,  24,  25,  25,  25,  26,  26,  26,  27,  27,  27,  27,
	  28,  28,  28,  29,  29,  29,  30,  30,  30,  31,  31,  32,
	  32,  32,  33,  33,  33,  34,  34,  35,  35,  35,  36,  36,
	  37,  37,  38,  38,  38,  39,  39,  40,  40,  41,  41,  42,
	  42,  43,  43,  44,  44,  45,  45,  46,  46,  47,  47,  48,
	  48,  49,  49,  50,  51,  51,  52,  52,  53,  54,  54,  55,
	  55,  56,  57,  57,  58,  59,  59,  60,  61,  62,  62,  63,
	  64,  64,  65,  66,  67,  67,  68,  69,  70,  71,  71,  72,
	  73,  74,  75,  76,  77,  77,  78,  79,  80,  81,  82,  83,
	  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
	  96,  98,  99, 100, 101, 102, 103, 105, 106, 107, 108, 109,
	 111, 112, 113, 115, 116, 117, 119, 120, 121, 123, 124, 126,
	 127, 129, 130, 132, 133, 135, 136, 138, 139, 141, 143, 144,
	 146, 148, 149, 151, 153, 155, 156, 158, 160, 162, 164, 166,
	 168, 169, 171, 173, 175, 177, 180, 182, 184, 186, 188, 190,
	 192, 195, 197, 199, 201, 204, 206, 209, 211, 213, 216, 218,
	 221, 223, 226, 229, 231, 234, 237, 239, 242, 245, 248, 251,
	 254, 257, 259, 262, 266, 269, 272, 275, 278, 281, 285, 288,
	 291, 295, 298, 301, 305, 308, 312, 316, 319, 323, 327, 330,
	 334, 338, 342, 346, 350, 354, 358, 362, 367, 371, 375, 379,
	 384, 388, 393, 397, 402, 407, 411, 416, 421, 426, 431, 436,
	 441, 446, 451, 456, 461, 467, 472, 478, 483, 489, 494, 500,
	 506, 512, 518, 524, 530, 536, 542, 548, 555, 561, 568, 574,
	 581, 588, 594, 601, 608, 615, 622, 630, 637, 644, 652, 659,
	 667, 675, 683, 690, 698, 707, 715, 723, 731, 740, 748, 757,
	 766, 775, 784, 793, 802, 811, 821, 830, 840, 849, 859, 869,
	 879, 889, 900, 910, 921, 931, 942, 953, 964, 975, 987, 998,
	1010, 1021, 1033, 1045, 1057, 1069, 1082, 1094, 1107, 1120, 1133, 1146,
	1159, 1173, 1186, 1200, 1214, 1228, 1242, 1256, 1271, 1286, 1301, 1316,
	1331, 1346, 1362, 1378, 1394, 1410, 1426, 1443, 1459, 1476, 1493, 1510,
	1528, 1546, 1564, 1582, 1600, 1619, 1637, 1656, 1675, 1695, 1714, 1734,
	1754, 1775, 1795, 1816, 1837, 1858, 1880, 1902, 1924, 1946, 1968, 1991,
	2014, 2038, 2061, 2085, 2109, 2134, 2158, 2183, 2209, 2234, 2260, 2286,
	2313, 2339, 2367, 2394, 2422, 2450, 2478, 2507, 2536, 2565, 2595, 2625,
	2655, 2686, 2717, 2749, 2780, 2813, 2845, 2878, 2912, 2945, 2979, 3014,
	3049, 3084, 3120, 3156, 3192, 3229, 3267, 3305, 3343, 3382, 3421, 3460,
	3500, 3541, 3582, 3623, 3665, 3708, 3751, 3794, 3838, 3883, 3928, 3973,
	4019,
};

#define IMX296_GAIN_Q4_MAX	4019

static const char * const imx296_test_pattern_menu[] = {
	"Disabled",
	"Multiple Pixels",
	"Sequence 1",
	"Sequence 2",
	"Gradient",
	"Row",
	"Column",
	"Cross",
	"Stripe",
	"Checks",
};

/*
 * Estado propio del driver. sensor_info va primero: VIN la recibe por to_state(sd) y no
 * sabe nada de lo demás.
 *
 * lock es también el candado del manejador de controles: s_ctrl siempre corre con él
 * tomado, y s_stream lo toma, así que las escrituras I2C de los controles y las del
 * arranque o la parada nunca se mezclan. Orden: lock y después el bus I2C (cci_lock).
 */
struct imx296 {
	struct sensor_info info;
	struct mutex lock;
	bool streaming;
	bool mono;
	int temperature;		/* m°C, última lectura */
	struct sensor_win_size *mode;	/* modo de los rangos actuales de VBLANK/HBLANK */
	/* clusters: exposure + again (mismo REGHOLD), hflip + vflip (mismo registro) */
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *again;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *test_pattern;
	struct v4l2_ctrl *temp;
	struct v4l2_ctrl *gain_q4;
};

static inline struct imx296 *to_imx296(struct sensor_info *info)
{
	return container_of(info, struct imx296, info);
}

static struct regval_list sensor_default_regs[] = {
};

/*
 * Registros comunes a todos los modos, RAW10, 1 carril, INCK = 54 MHz.
 * Se escriben con el sensor en standby. sensor_write_array() agrupa direcciones
 * consecutivas en una sola ráfaga I2C.
 */
static struct regval_list sensor_common_regs[] = {
	/* imx296_init_table: datos del fabricante sin documentar. */
	/* La primera escritura activa CSI-2. */
	{0x3005, 0xf0},
	{0x309e, 0x04},
	{0x30a0, 0x04},
	{0x30a1, 0x3c},
	{0x30a4, 0x5f},
	{0x30a8, 0x91},
	{0x30ac, 0x28},
	{0x30af, 0x0b},		/* Raspberry Pi; el kernel principal usa 0x09 */
	{0x30df, 0x00},
	{0x3165, 0x00},
	{0x3169, 0x10},
	{0x316a, 0x02},
	{0x31c8, 0xf3},		/* relacionado con la exposición */
	{0x31d0, 0xf4},		/* relacionado con la exposición */
	{0x321a, 0x00},
	{0x3226, 0x02},
	{0x3256, 0x01},
	{0x3541, 0x72},
	{0x3516, 0x77},
	{0x350b, 0x7f},
	{0x3758, 0xa3},
	{0x3759, 0x00},
	{0x375a, 0x85},
	{0x375b, 0x00},
	{0x3832, 0xf5},
	{0x3833, 0x00},
	{0x38a2, 0xf6},
	{0x38a3, 0x00},
	{0x3a00, 0x80},
	{0x3d48, 0xa3},
	{0x3d49, 0x00},
	{0x3d4a, 0x85},
	{0x3d4b, 0x00},
	{0x400e, 0x58},
	{0x4014, 0x1c},
	{0x4041, 0x2a},
	{0x40a2, 0x06},
	{0x40c1, 0xf6},
	{0x40c7, 0x0f},
	{0x40c8, 0x00},
	{0x4174, 0x00},

	/* imx296_setup(), parte común. Recorte, binning y VMAX van en cada modo. */
	{0x3014, 0x4c},		/* HMAX = 1100 */
	{0x3015, 0x04},
	{0x3089, 0xb0},		/* INCKSEL0..3 para 54 MHz */
	{0x308a, 0x0f},
	{0x308b, 0xb0},
	{0x308c, 0x0c},
	/* GAINDLY = 1 cuadro (Raspberry Pi): ganancia y SHS1 entran en el mismo cuadro */
	{0x3212, 0x09},
	{0x4114, 0xc5},		/* GTTABLENUM */
	{0x418c, 0xa8},		/* CTRL418C = 168 para 54 MHz */
	/*
	 * CTRL0E (espejo), VMAX, SHS1 (exposición), GAIN y el patrón de prueba (con BLKLEVEL)
	 * se escriben en sensor_reg_init() con los valores vigentes de los controles.
	 */
};

/* 1456x1088: sin recorte ni binning. */
static struct regval_list sensor_1456x1088_regs[] = {
	{0x3300, 0x00},		/* FID0_ROI: sin recorte */
	{0x4182, 0x40},		/* MIPIC_AREA3W = 1088 (Raspberry Pi: alto del recorte) */
	{0x4183, 0x04},
	{0x300d, 0x00},		/* CTRL0D: WINMODE_ALL */
};

/* 1456x544: recorte vertical centrado (filas 272..815); la línea dura lo mismo. */
static struct regval_list sensor_1456x544_regs[] = {
	{0x3300, 0x03},		/* FID0_ROI: ROIH1ON | ROIV1ON */
	{0x3310, 0x00},		/* FID0_ROIPH1 = 0 */
	{0x3311, 0x00},
	{0x3312, 0x10},		/* FID0_ROIPV1 = 272 */
	{0x3313, 0x01},
	{0x3314, 0xb0},		/* FID0_ROIWH1 = 1456 */
	{0x3315, 0x05},
	{0x3316, 0x20},		/* FID0_ROIWV1 = 544 */
	{0x3317, 0x02},
	{0x4182, 0x20},		/* MIPIC_AREA3W = 544 */
	{0x4183, 0x02},
	{0x300d, 0x00},		/* CTRL0D: WINMODE_ALL */
};

/*
 * 728x544: binning 2x2 sobre el campo completo (suma horizontal y binning vertical en el
 * nodo de difusión flotante). El kernel principal lo admite solo sin recorte, con la nota
 * "should be double-checked"; Raspberry Pi lo quitó. Experimental.
 */
static struct regval_list sensor_728x544_bin_regs[] = {
	{0x3300, 0x00},		/* FID0_ROI: sin recorte */
	{0x4182, 0x40},		/* MIPIC_AREA3W = 1088 (sin recorte) */
	{0x4183, 0x04},
	{0x300d, 0x22},		/* CTRL0D: HADD_ON_BINNING | WINMODE_FD_BINNING */
};

static struct regval_list sensor_fmt_raw[] = {
};

/* Escribe un registro multibyte con el byte bajo primero. */
static int imx296_write_multi(struct v4l2_subdev *sd, addr_type reg, u32 val, int len)
{
	int i, ret;

	for (i = 0; i < len; i++) {
		ret = sensor_write(sd, reg + i, (val >> (8 * i)) & 0xff);
		if (ret)
			return ret;
	}
	return 0;
}

static unsigned int imx296_height(struct sensor_info *info)
{
	return info->current_wins ? info->current_wins->height : IMX296_HEIGHT;
}

/*
 * Exposición, ganancia y VMAX con los valores dados, en un solo bloque REGHOLD: el sensor
 * los toma juntos al comienzo del cuadro siguiente. Con GAINDLY = 1 cuadro, la ganancia
 * coincide con el SHS1. La exposición se recorta a VMAX - IMX296_SHS1_MIN.
 * Deja en info->exp (líneas * 16) e info->gain (Q4) los valores aplicados, en las
 * unidades de VIN.
 */
static int imx296_write_timing(struct imx296 *priv, unsigned int exposure,
			       unsigned int again, unsigned int vblank)
{
	struct sensor_info *info = &priv->info;
	struct v4l2_subdev *sd = &info->sd;
	unsigned int vmax = min_t(unsigned int, imx296_height(info) + vblank, IMX296_VMAX_MAX);
	unsigned int lines = clamp(exposure, 1U, vmax - IMX296_SHS1_MIN);
	int ret, ret_hold;

	again = min_t(unsigned int, again, IMX296_GAIN_REG_MAX);
	ret = sensor_write(sd, IMX296_CTRL08, 0x01);
	if (!ret)
		ret = imx296_write_multi(sd, IMX296_VMAX, vmax, 3);
	if (!ret)
		ret = imx296_write_multi(sd, IMX296_SHS1, vmax - lines, 3);
	if (!ret)
		ret = imx296_write_multi(sd, IMX296_GAIN, again, 2);
	/* REGHOLD se suelta siempre, aunque algo haya fallado antes. */
	ret_hold = sensor_write(sd, IMX296_CTRL08, 0x00);
	if (ret || ret_hold)
		return ret ? ret : ret_hold;

	info->exp = lines << 4;
	info->gain = imx296_gain_q4[again];
	sensor_dbg("VMAX %u, exposure %u lines (SHS1 %u), gain %u\n",
		   vmax, lines, vmax - lines, again);
	return 0;
}

static int imx296_write_flip(struct imx296 *priv, bool hflip, bool vflip)
{
	return sensor_write(&priv->info.sd, IMX296_CTRL0E, (vflip ? 0x01 : 0) | (hflip ? 0x02 : 0));
}

/* Valores del kernel principal: PGDATA1 = 0x300 y PGDATA2 = 0x100, sin nivel de negro. */
static int imx296_write_test_pattern(struct imx296 *priv, int pattern)
{
	struct v4l2_subdev *sd = &priv->info.sd;
	int ret;

	if (!pattern) {
		ret = sensor_write(sd, IMX296_PGCTRL, IMX296_PGCTRL_CLKEN);
		if (!ret)
			ret = imx296_write_multi(sd, IMX296_BLKLEVEL, IMX296_BLKLEVEL_DEF, 2);
		if (!ret)
			ret = sensor_write(sd, IMX296_BLKLEVELAUTO, IMX296_BLKLEVELAUTO_ON);
		return ret;
	}

	ret = imx296_write_multi(sd, IMX296_PGHPOS, 8, 2);
	if (!ret)
		ret = imx296_write_multi(sd, IMX296_PGVPOS, 8, 2);
	if (!ret)
		ret = sensor_write(sd, IMX296_PGHPSTEP, 8);
	if (!ret)
		ret = sensor_write(sd, IMX296_PGVPSTEP, 8);
	if (!ret)
		ret = sensor_write(sd, IMX296_PGHPNUM, 100);
	if (!ret)
		ret = sensor_write(sd, IMX296_PGVPNUM, 100);
	if (!ret)
		ret = imx296_write_multi(sd, IMX296_PGDATA1, 0x300, 2);
	if (!ret)
		ret = imx296_write_multi(sd, IMX296_PGDATA2, 0x100, 2);
	if (!ret)
		ret = sensor_write(sd, IMX296_PGHGSTEP, 0);
	if (!ret)
		ret = imx296_write_multi(sd, IMX296_BLKLEVEL, 0, 2);
	if (!ret)
		ret = sensor_write(sd, IMX296_BLKLEVELAUTO, IMX296_BLKLEVELAUTO_OFF);
	if (!ret)
		ret = sensor_write(sd, IMX296_PGCTRL, IMX296_PGCTRL_REGEN | IMX296_PGCTRL_CLKEN |
				   IMX296_PGCTRL_MODE(pattern - 1));
	return ret;
}

/*
 * Temperatura del sensor (imx296_read_temperature del kernel principal). Solo funciona
 * fuera de standby. T = 246,312 - 0,304 * TMDOUT grados.
 */
static int imx296_read_temperature(struct v4l2_subdev *sd, int *temp)
{
	data_type lo = 0, hi = 0;
	int ret;

	ret = sensor_write(sd, IMX296_TMDCTRL, 0x01);
	if (!ret)
		ret = sensor_read(sd, IMX296_TMDOUT, &lo);
	if (!ret)
		ret = sensor_read(sd, IMX296_TMDOUT + 1, &hi);
	sensor_write(sd, IMX296_TMDCTRL, 0x00);
	if (ret)
		return ret;

	*temp = 246312 - 304 * ((((hi & 0xff) << 8) | (lo & 0xff)) & IMX296_TMDOUT_MASK);
	return 0;
}

/*
 * Exposición y ganancia por cuadro (3A de VIN, unidades Q4). Pasa por los controles para
 * que queden coherentes. Solo lo usaría el servidor de ISP de Allwinner, que no corre.
 */
static int sensor_s_exp_gain(struct v4l2_subdev *sd,
			     struct sensor_exp_gain *exp_gain)
{
	struct imx296 *priv = to_imx296(to_state(sd));
	int lo = 0, hi = IMX296_GAIN_REG_MAX, mid, q4, ret;

	/* Primer paso de 0,1 dB que alcanza la ganancia pedida. */
	q4 = clamp_t(int, exp_gain->gain_val, 16, IMX296_GAIN_Q4_MAX);
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (imx296_gain_q4[mid] < q4)
			lo = mid + 1;
		else
			hi = mid;
	}

	ret = v4l2_ctrl_s_ctrl(priv->again, lo);
	if (!ret)
		ret = v4l2_ctrl_s_ctrl(priv->exposure,
				       clamp_t(int, exp_gain->exp_val >> 4, 1, IMX296_EXP_MAX));
	return ret;
}

/*
 * Encendido. En los módulos de Raspberry Pi, el pin de encendido del conector
 * (pwdn en el DT: PE6 / PE10, activo alto) habilita los reguladores de la
 * cámara; el overlay oficial espera 500 ms (startup-delay-us = 500000).
 * No se usa MCLK: la cámara tiene su propio oscilador de 54 MHz.
 */
static int sensor_power(struct v4l2_subdev *sd, int on)
{
	struct imx296 *priv = to_imx296(to_state(sd));

	switch (on) {
	case STBY_ON:
		/*
		 * Sin cci_lock: en modo I2C, cci_lock() toma el candado del adaptador
		 * (i2c_lock_bus) y sensor_write() termina en i2c_transfer(), que pide el
		 * mismo mutex no recursivo -> deadlock. Por eso imx219.c tiene comentado
		 * su sensor_s_sw_stby(). Solo se llega aquí por el ioctl de standby de VIN.
		 */
		sensor_dbg("STBY_ON!\n");
		sensor_write(sd, IMX296_CTRL0A, 0x01);
		sensor_write(sd, IMX296_CTRL00, 0x01);
		break;
	case STBY_OFF:
		/* Se sale de standby en sensor_s_stream(), después de cargar la tabla. */
		sensor_dbg("STBY_OFF!\n");
		break;
	case REG_ON:
		/* Solo lo usa VIN con CONFIG_VIN_INIT_MELIS; no hay reguladores que encender. */
		break;
	case PWR_ON:
		sensor_dbg("PWR_ON!\n");
		cci_lock(sd);
		vin_gpio_set_status(sd, PWDN, 1);
		vin_gpio_write(sd, PWDN, CSI_GPIO_LOW);
		usleep_range(1000, 1200);
		vin_gpio_write(sd, PWDN, CSI_GPIO_HIGH);
		cci_unlock(sd);
		msleep(500);
		break;
	case PWR_OFF:
		sensor_dbg("PWR_OFF!\n");
		/* Apagado: los controles dejan de escribir. lock antes que el bus (ver struct imx296). */
		mutex_lock(&priv->lock);
		priv->streaming = false;
		mutex_unlock(&priv->lock);
		cci_lock(sd);
		vin_gpio_write(sd, PWDN, CSI_GPIO_LOW);
		vin_gpio_set_status(sd, PWDN, 0);
		cci_unlock(sd);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* La cámara no expone RESET (XCLR va al mismo habilitador que pwdn). */
static int sensor_reset(struct v4l2_subdev *sd, u32 val)
{
	return 0;
}

/*
 * SENSOR_INFO (0x3148) solo se lee fuera de standby. Se compara el número de
 * modelo ((valor >> 6) & 0x1ff == 296), como imx296_identify_model(), y se
 * aprovecha para leer la temperatura.
 */
static int sensor_detect(struct v4l2_subdev *sd)
{
	struct imx296 *priv = to_imx296(to_state(sd));
	data_type lo = 0, hi = 0;
	unsigned int info, model;
	int ret, temp;

	ret = sensor_write(sd, IMX296_CTRL00, 0x00);
	if (ret) {
		sensor_err("no answer when leaving standby (%d)\n", ret);
		return -ENODEV;
	}
	usleep_range(2000, 5000);

	ret = sensor_read(sd, IMX296_SENSOR_INFO, &lo);
	if (!ret)
		ret = sensor_read(sd, IMX296_SENSOR_INFO + 1, &hi);
	if (!ret && !imx296_read_temperature(sd, &temp))
		priv->temperature = temp;
	sensor_write(sd, IMX296_CTRL00, 0x01);
	if (ret) {
		sensor_err("cannot read SENSOR_INFO (%d)\n", ret);
		return -ENODEV;
	}

	info = ((hi & 0xff) << 8) | (lo & 0xff);
	model = (info >> 6) & 0x1ff;
	if (model != 296) {
		sensor_err("not an IMX296: SENSOR_INFO = 0x%04x\n", info);
		return -ENODEV;
	}

	priv->mono = info & IMX296_SENSOR_INFO_MONO;
	sensor_print("Sony IMX296%s detected (SENSOR_INFO = 0x%04x, %d.%d C)\n",
		     priv->mono ? "LL (mono)" : "LQ (color)", info,
		     priv->temperature / 1000, abs(priv->temperature % 1000) / 100);
	if (!priv->mono)
		sensor_print("color sensor: format declared as RGGB, not verified (with flips the order changes)\n");
	return 0;
}

static int sensor_init(struct v4l2_subdev *sd, u32 val)
{
	struct sensor_info *info = to_state(sd);
	int ret;

	/* sensor_detect() ya explica el fallo; VIN agrega "registering ..., No such device!". */
	ret = sensor_detect(sd);
	if (ret)
		return ret;

	info->focus_status = 0;
	info->low_speed = 0;
	info->width = IMX296_WIDTH;
	info->height = IMX296_HEIGHT;
	info->hflip = 0;
	info->vflip = 0;
	/*
	 * Con tpf = 1/60, sensor_find_frame_size() solo considera los modos de 60 fps y nunca
	 * elegiría los de 100. Sin coincidencia, VIN elige solo por tamaño. tpf solo se compara
	 * (nunca se divide por él); un VIDIOC_S_PARM de la aplicación lo reemplaza.
	 */
	info->tpf.numerator = 1;
	info->tpf.denominator = 0;
	info->preview_first_flag = 1;

	return 0;
}

static long sensor_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct sensor_info *info = to_state(sd);
	int ret = 0;

	switch (cmd) {
	case GET_CURRENT_WIN_CFG:
		if (info->current_wins) {
			memcpy(arg, info->current_wins, sizeof(*info->current_wins));
		} else {
			sensor_err("no current window\n");
			ret = -EINVAL;
		}
		break;
	case SET_FPS:
		break;
	case VIDIOC_VIN_SENSOR_EXP_GAIN:
		ret = sensor_s_exp_gain(sd, (struct sensor_exp_gain *)arg);
		break;
	case VIDIOC_VIN_SENSOR_CFG_REQ:
		sensor_cfg_req(sd, (struct sensor_config *)arg);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

/*
 * Las cámaras del proyecto son mono (IMX296LL) y VIN no reconoce MEDIA_BUS_FMT_Y10_1X10:
 * se declara SRGGB10, que tiene el mismo empaquetado RAW10, y los datos se leen como gris.
 * Para la variante color (LQ), RGGB sin espejo sale de la tabla de Raspberry Pi (el kernel
 * principal dice BGGR) y no está verificado con una imagen real.
 */
static struct sensor_format_struct sensor_formats[] = {
	{
		.desc = "Raw RGB Bayer",
		.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.regs = sensor_fmt_raw,
		.regs_size = ARRAY_SIZE(sensor_fmt_raw),
		.bpp = 1
	},
};

#define N_FMTS ARRAY_SIZE(sensor_formats)

/*
 * Modos. VIN elige el de tamaño más cercano que sea >= al pedido (sensor_find_frame_size).
 * vts es el VMAX por defecto, que da fps_fixed. VIN pasa fps_fixed al ISP y al TDM, que lo
 * usa para sus búferes y su tasa de envío: por eso VBLANK no puede bajar de vts - alto (los
 * fps solo se pueden bajar).
 */
static struct sensor_win_size sensor_win_sizes[] = {
	{
		.width = IMX296_WIDTH,
		.height = IMX296_HEIGHT,
		.hoffset = 0,
		.voffset = 0,
		.hts = IMX296_HTS,
		.vts = IMX296_VTS_60FPS,
		.pclk = IMX296_PCLK,
		.mipi_bps = IMX296_MIPI_BPS,
		.fps_fixed = 60,
		.bin_factor = 1,
		.intg_min = 1 << 4,
		.intg_max = (IMX296_VTS_60FPS - IMX296_SHS1_MIN) << 4,
		.gain_min = 1 << 4,
		.gain_max = IMX296_GAIN_Q4_MAX,
		.regs = sensor_1456x1088_regs,
		.regs_size = ARRAY_SIZE(sensor_1456x1088_regs),
		.set_size = NULL,
	}, {
		.width = IMX296_WIDTH,
		.height = IMX296_HEIGHT / 2,
		.hoffset = 0,
		.voffset = 0,
		.hts = IMX296_HTS,
		.vts = IMX296_VTS_100FPS,
		.pclk = IMX296_PCLK,
		.mipi_bps = IMX296_MIPI_BPS,
		.fps_fixed = 100,
		.bin_factor = 1,
		.intg_min = 1 << 4,
		.intg_max = (IMX296_VTS_100FPS - IMX296_SHS1_MIN) << 4,
		.gain_min = 1 << 4,
		.gain_max = IMX296_GAIN_Q4_MAX,
		.regs = sensor_1456x544_regs,
		.regs_size = ARRAY_SIZE(sensor_1456x544_regs),
		.set_size = NULL,
	}, {
		.width = IMX296_WIDTH / 2,
		.height = IMX296_HEIGHT / 2,
		.hoffset = 0,
		.voffset = 0,
		.hts = IMX296_HTS,
		.vts = IMX296_VTS_100FPS,
		.pclk = IMX296_PCLK,
		.mipi_bps = IMX296_MIPI_BPS,
		.fps_fixed = 100,
		.bin_factor = 2,
		.intg_min = 1 << 4,
		.intg_max = (IMX296_VTS_100FPS - IMX296_SHS1_MIN) << 4,
		.gain_min = 1 << 4,
		.gain_max = IMX296_GAIN_Q4_MAX,
		.regs = sensor_728x544_bin_regs,
		.regs_size = ARRAY_SIZE(sensor_728x544_bin_regs),
		.set_size = NULL,
	},
};

#define N_WIN_SIZES (ARRAY_SIZE(sensor_win_sizes))

/*
 * Ajusta los rangos de VBLANK y HBLANK al modo actual. Si el modo cambió, VBLANK vuelve al
 * valor por defecto del modo (sus fps nominales). Llamar con priv->lock tomado.
 */
static void imx296_update_mode_ctrls(struct imx296 *priv)
{
	struct sensor_win_size *ws = priv->info.current_wins;
	unsigned int vblank_min, hblank;

	if (!ws || ws == priv->mode)
		return;

	vblank_min = ws->vts - ws->height;
	hblank = ws->hts - ws->width;
	__v4l2_ctrl_modify_range(priv->vblank, vblank_min, IMX296_VMAX_MAX - ws->height, 1,
				 vblank_min);
	__v4l2_ctrl_s_ctrl(priv->vblank, vblank_min);
	__v4l2_ctrl_modify_range(priv->hblank, hblank, hblank, 1, hblank);
	priv->mode = ws;
}

/* VIN elige el modo en sensor_set_fmt(); después se ajustan los controles que dependen de él. */
static int imx296_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx296 *priv = to_imx296(to_state(sd));
	int ret;

	ret = sensor_set_fmt(sd, state, fmt);
	if (ret || fmt->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return ret;

	mutex_lock(&priv->lock);
	imx296_update_mode_ctrls(priv);
	mutex_unlock(&priv->lock);
	return 0;
}

/* Llamar con priv->lock tomado. */
static int sensor_reg_init(struct imx296 *priv)
{
	struct sensor_info *info = &priv->info;
	struct v4l2_subdev *sd = &info->sd;
	struct sensor_format_struct *sensor_fmt = info->fmt;
	struct sensor_win_size *wsize = info->current_wins;
	int ret;

	if (!wsize || !sensor_fmt) {
		sensor_err("format not initialized\n");
		return -EINVAL;
	}
	/* Por si el modo cambió sin pasar por imx296_set_fmt(). */
	imx296_update_mode_ctrls(priv);

	ret = sensor_write_array(sd, sensor_default_regs,
				 ARRAY_SIZE(sensor_default_regs));
	if (!ret)
		ret = sensor_write_array(sd, sensor_common_regs, ARRAY_SIZE(sensor_common_regs));
	if (!ret)
		ret = sensor_write_array(sd, wsize->regs, wsize->regs_size);
	if (ret < 0) {
		sensor_err("cannot write mode registers (%d)\n", ret);
		return ret;
	}

	sensor_write_array(sd, sensor_fmt->regs, sensor_fmt->regs_size);

	if (wsize->set_size)
		wsize->set_size(sd);

	/*
	 * Valores vigentes de los controles. No van en las tablas para no pisar lo que se haya
	 * fijado antes de arrancar. El sensor sigue en standby, así que entran desde el primer
	 * cuadro.
	 */
	ret = imx296_write_flip(priv, priv->hflip->cur.val, priv->vflip->cur.val);
	if (!ret)
		ret = imx296_write_test_pattern(priv, priv->test_pattern->cur.val);
	if (!ret)
		ret = imx296_write_timing(priv, priv->exposure->cur.val, priv->again->cur.val,
					  priv->vblank->cur.val);
	if (ret) {
		sensor_err("cannot apply controls (%d)\n", ret);
		return ret;
	}

	info->width = wsize->width;
	info->height = wsize->height;

	sensor_print("stream %ux%u, %u fps, flip h%d v%d, exposure %u lines (%u us), gain %d (0.1 dB)%s\n",
		     wsize->width, wsize->height,
		     IMX296_PCLK / (IMX296_HTS * (wsize->height + priv->vblank->cur.val)),
		     priv->hflip->cur.val, priv->vflip->cur.val, info->exp >> 4,
		     (info->exp >> 4) * IMX296_LINE_NS / 1000, priv->again->cur.val,
		     priv->test_pattern->cur.val ? ", test pattern" : "");
	return 0;
}

/*
 * Arranque (imx296_stream_on de Raspberry Pi): salir de standby, esperar a que
 * se estabilice, fijar el modo de disparo y por último XMSTA = 0. El disparo
 * se configura antes de XMSTA para que el sensor no arranque en modo libre.
 */
static int sensor_start(struct v4l2_subdev *sd)
{
	int ret;

	ret = sensor_write(sd, IMX296_CTRL00, 0x00);
	usleep_range(2000, 5000);
	ret |= sensor_write(sd, IMX296_CTRL0B, trigger_mode.val == 1 ? 0x01 : 0x00);
	ret |= sensor_write(sd, IMX296_LOWLAGTRG, trigger_mode.val == 1 ? 0x01 : 0x00);
	ret |= sensor_write(sd, IMX296_CTRL0A, 0x00);

	return ret ? -EIO : 0;
}

/*
 * Parada en orden inverso. imx219/imx290 no la tienen: el sensor seguiría transmitiendo.
 * Siempre devuelve 0: si VIN recibe un error en s_stream(0), deja de apagar el resto
 * de la cadena (MIPI/CSI). Un fallo aquí solo se registra.
 */
static int sensor_stop(struct v4l2_subdev *sd)
{
	int ret;

	ret = sensor_write(sd, IMX296_CTRL0A, 0x01);
	ret |= sensor_write(sd, IMX296_CTRL00, 0x01);
	if (ret)
		sensor_err("cannot stop streaming (%d)\n", ret);

	return 0;
}

static int sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx296 *priv = to_imx296(to_state(sd));
	struct sensor_info *info = &priv->info;
	int ret = 0, temp;

	if (!info->current_wins || !info->fmt) {
		sensor_err("%s: format not initialized\n", __func__);
		return enable ? -EINVAL : 0;
	}

	sensor_dbg("%s on = %d, %ux%u 0x%x, trigger_mode = %d\n", __func__, enable,
		   info->current_wins->width, info->current_wins->height,
		   info->fmt->mbus_code, trigger_mode.val);

	mutex_lock(&priv->lock);
	if (!enable) {
		/* La temperatura se lee antes de volver a standby. */
		if (priv->streaming && !imx296_read_temperature(sd, &temp)) {
			priv->temperature = temp;
			sensor_print("stream off, sensor %d.%d C\n", temp / 1000, abs(temp % 1000) / 100);
		}
		priv->streaming = false;
		ret = sensor_stop(sd);
	} else {
		ret = sensor_reg_init(priv);
		if (!ret)
			ret = sensor_start(sd);
		priv->streaming = !ret;
	}
	mutex_unlock(&priv->lock);
	return ret;
}

/* VIN usa num_data_lanes como máscara de flags (platform_cfg.h): 1 carril, canal virtual 0. */
static int sensor_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_mbus_config *cfg)
{
	cfg->type = V4L2_MBUS_CSI2_DPHY;
	cfg->bus.mipi_csi2.num_data_lanes = V4L2_MBUS_CSI2_1_LANE | V4L2_MBUS_CSI2_CHANNEL_0;
	return 0;
}

/* Corre con priv->lock tomado (es el candado del manejador). */
static int sensor_g_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx296 *priv = container_of(ctrl->handler, struct imx296, info.handler);
	int temp;

	switch (ctrl->id) {
	case IMX296_CID_TEMPERATURE:
		/* Fuera de standby se mide; si no, queda la última lectura. */
		if (priv->streaming && !imx296_read_temperature(&priv->info.sd, &temp))
			priv->temperature = temp;
		ctrl->val = priv->temperature;
		return 0;
	case V4L2_CID_GAIN:
		/* Solo lectura, para VIN: el nodo de video lee la ganancia del sensor en Q4. */
		ctrl->val = imx296_gain_q4[priv->again->cur.val];
		return 0;
	}
	return -EINVAL;
}

/*
 * Corre con priv->lock tomado. Sin transmisión solo se guarda el valor: el sensor puede estar
 * apagado o en standby, y sensor_reg_init() aplica todos los controles al arrancar.
 * En los clusters llega el control maestro; los demás miembros tienen su valor nuevo en ->val.
 */
static int sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx296 *priv = container_of(ctrl->handler, struct imx296, info.handler);

	if (!priv->streaming)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:		/* cluster: exposure + analogue_gain */
		return imx296_write_timing(priv, priv->exposure->val, priv->again->val,
					   priv->vblank->cur.val);
	case V4L2_CID_VBLANK:
		/* VMAX cambia: SHS1 se recalcula en el mismo bloque REGHOLD. */
		return imx296_write_timing(priv, priv->exposure->cur.val, priv->again->cur.val,
					   ctrl->val);
	case V4L2_CID_HFLIP:		/* cluster: hflip + vflip */
		return imx296_write_flip(priv, priv->hflip->val, priv->vflip->val);
	case V4L2_CID_TEST_PATTERN:
		return imx296_write_test_pattern(priv, ctrl->val);
	}
	return -EINVAL;
}

/* ----------------------------------------------------------------------- */

static const struct v4l2_ctrl_ops sensor_ctrl_ops = {
	.g_volatile_ctrl = sensor_g_ctrl,
	.s_ctrl = sensor_s_ctrl,
};

static const struct v4l2_ctrl_config imx296_temperature_ctrl = {
	.ops = &sensor_ctrl_ops,
	.id = IMX296_CID_TEMPERATURE,
	.name = "Temperature (mC)",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	.min = -65000,
	.max = 247000,
	.step = 1,
	.def = 0,
};

static const struct v4l2_subdev_core_ops sensor_core_ops = {
	.reset = sensor_reset,
	.init = sensor_init,
	.s_power = sensor_power,
	.ioctl = sensor_ioctl,
#if IS_ENABLED(CONFIG_COMPAT)
	.compat_ioctl32 = sensor_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops sensor_video_ops = {
	.s_stream = sensor_s_stream,
};

static const struct v4l2_subdev_pad_ops sensor_pad_ops = {
	.enum_mbus_code = sensor_enum_mbus_code,
	.enum_frame_size = sensor_enum_frame_size,
	.get_fmt = sensor_get_fmt,
	.set_fmt = imx296_set_fmt,
	.get_mbus_config = sensor_g_mbus_config,
};

static const struct v4l2_subdev_ops sensor_ops = {
	.core = &sensor_core_ops,
	.video = &sensor_video_ops,
	.pad = &sensor_pad_ops,
};

/* ----------------------------------------------------------------------- */
static struct cci_driver cci_drv[] = {
	{
		.name = SENSOR_NAME,
		.addr_width = CCI_BITS_16,
		.data_width = CCI_BITS_8,
	}, {
		.name = SENSOR_NAME_2,
		.addr_width = CCI_BITS_16,
		.data_width = CCI_BITS_8,
	}
};

/*
 * Controles con las unidades del kernel principal: exposición en líneas, ganancia analógica
 * en pasos de 0,1 dB (el valor del registro), blanking en líneas y píxeles. Los valores
 * iniciales salen de los parámetros del módulo. VBLANK y HBLANK quedan para el modo 0
 * hasta el primer VIDIOC_S_FMT (imx296_update_mode_ctrls).
 */
static int sensor_init_controls(struct imx296 *priv)
{
	struct sensor_info *info = &priv->info;
	struct v4l2_ctrl_handler *handler = &info->handler;
	struct sensor_win_size *ws = &sensor_win_sizes[0];
	const struct v4l2_ctrl_ops *ops = &sensor_ctrl_ops;
	u64 lines = ((u64)exposure_us.val * 1000 + IMX296_LINE_NS / 2) / IMX296_LINE_NS;
	int flip = rotation.val == 180;

	v4l2_ctrl_handler_init(handler, 11);
	handler->lock = &priv->lock;

	priv->exposure = v4l2_ctrl_new_std(handler, ops, V4L2_CID_EXPOSURE, 1, IMX296_EXP_MAX,
					   1, clamp_t(u64, lines, 1, IMX296_EXP_MAX));
	priv->again = v4l2_ctrl_new_std(handler, ops, V4L2_CID_ANALOGUE_GAIN, 0,
					IMX296_GAIN_REG_MAX, 1, gain_db10.val);
	priv->hflip = v4l2_ctrl_new_std(handler, ops, V4L2_CID_HFLIP, 0, 1, 1, flip);
	priv->vflip = v4l2_ctrl_new_std(handler, ops, V4L2_CID_VFLIP, 0, 1, 1, flip);
	priv->vblank = v4l2_ctrl_new_std(handler, ops, V4L2_CID_VBLANK, ws->vts - ws->height,
					 IMX296_VMAX_MAX - ws->height, 1, ws->vts - ws->height);
	priv->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK, ws->hts - ws->width,
					 ws->hts - ws->width, 1, ws->hts - ws->width);
	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE, IMX296_PCLK, IMX296_PCLK, 1,
			  IMX296_PCLK);
	priv->test_pattern = v4l2_ctrl_new_std_menu_items(handler, ops, V4L2_CID_TEST_PATTERN,
							  ARRAY_SIZE(imx296_test_pattern_menu) - 1,
							  0, 0, imx296_test_pattern_menu);
	priv->temp = v4l2_ctrl_new_custom(handler, &imx296_temperature_ctrl, NULL);
	/*
	 * vin_g_volatile_ctrl() (nodo de video) lee V4L2_CID_GAIN del sensor en Q4; sin este
	 * control mostraba basura. Solo lectura: la ganancia se cambia con analogue_gain.
	 */
	priv->gain_q4 = v4l2_ctrl_new_std(handler, ops, V4L2_CID_GAIN, 16, IMX296_GAIN_Q4_MAX, 1,
					  imx296_gain_q4[gain_db10.val]);

	if (handler->error) {
		int ret = handler->error;

		v4l2_ctrl_handler_free(handler);
		return ret;
	}

	priv->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	priv->gain_q4->flags |= V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE;
	/* Exposición y ganancia en el mismo bloque REGHOLD; los dos espejos, en el mismo registro. */
	v4l2_ctrl_cluster(2, &priv->exposure);
	v4l2_ctrl_cluster(2, &priv->hflip);
	priv->mode = ws;

	info->sd.ctrl_handler = handler;
	return 0;
}

/* Solo se usa en el modo CCI de VIN (CONFIG_CSI_CCI), desactivado en la A733. */
static int sensor_dev_id;

/* Índice en cci_drv[] de la instancia con ese nombre de cliente I2C, o -1. */
static int imx296_instance(struct i2c_client *client)
{
	int i;

	for (i = 0; i < SENSOR_NUM; i++) {
		if (!strcmp(cci_drv[i].name, client->name))
			return i;
	}
	return -1;
}

/* Solo kernel 6.6 (rama orange-pi-6.6-sun60iw2): probe sin i2c_device_id, remove void. */
static int sensor_probe(struct i2c_client *client)
{
	struct v4l2_subdev *sd;
	struct sensor_info *info;
	struct imx296 *priv;
	int i, ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	info = &priv->info;
	sd = &info->sd;
	/* Antes que nada: sensor_registered() (cci_helper.c) toma info->lock. */
	mutex_init(&info->lock);
	mutex_init(&priv->lock);

	if (client) {
		i = imx296_instance(client);
		if (i < 0) {
			kfree(priv);
			return -ENODEV;
		}
	} else {
		i = sensor_dev_id++;
	}
	cci_dev_probe_helper(sd, client, &sensor_ops, &cci_drv[i]);

	/* Sin controles, sensor_reg_init() y s_ctrl usarían punteros NULL: el probe falla. */
	ret = sensor_init_controls(priv);
	if (ret) {
		sensor_err("cannot create controls (%d)\n", ret);
		cci_dev_remove_helper(client, &cci_drv[i]);
		mutex_destroy(&priv->lock);
		kfree(priv);
		return ret;
	}

#if IS_ENABLED(CONFIG_SAME_I2C)
	info->sensor_i2c_addr = I2C_ADDR >> 1;
#endif
	info->fmt = &sensor_formats[0];
	info->fmt_pt = &sensor_formats[0];
	info->win_pt = &sensor_win_sizes[0];
	/*
	 * Se fija un modo desde ya para que ningún camino (get_fmt sobre /dev/v4l-subdevN,
	 * sensor_get_exp, sensor_get_clk, s_stream) encuentre NULL antes del primer
	 * VIDIOC_S_FMT. sensor_set_fmt() lo cambia según el tamaño pedido.
	 */
	info->current_wins = &sensor_win_sizes[0];
	info->fmt_num = N_FMTS;
	info->win_size_num = N_WIN_SIZES;
	info->sensor_field = V4L2_FIELD_NONE;
	info->af_first_flag = 1;
	info->exp = priv->exposure->cur.val << 4;
	info->gain = imx296_gain_q4[gain_db10.val];

	return 0;
}

static void sensor_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd;
	struct imx296 *priv;
	int i;

	if (client) {
		i = imx296_instance(client);
		if (i < 0)
			return;
		sd = cci_dev_remove_helper(client, &cci_drv[i]);
	} else {
		sd = cci_dev_remove_helper(client, &cci_drv[sensor_dev_id++]);
	}
	priv = to_imx296(to_state(sd));
	/* cci_dev_remove_helper() ya desregistró el subdev; los controles se pueden liberar. */
	v4l2_ctrl_handler_free(&priv->info.handler);
	mutex_destroy(&priv->lock);
	kfree(priv);
}

static const struct i2c_device_id sensor_id[] = {
	{SENSOR_NAME, 0},
	{}
};

static const struct i2c_device_id sensor_id_2[] = {
	{SENSOR_NAME_2, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, sensor_id);
MODULE_DEVICE_TABLE(i2c, sensor_id_2);

static struct i2c_driver sensor_driver[] = {
	{
		.driver = {
			   .owner = THIS_MODULE,
			   .name = SENSOR_NAME,
			   },
		.probe = sensor_probe,
		.remove = sensor_remove,
		.id_table = sensor_id,
	}, {
		.driver = {
			   .owner = THIS_MODULE,
			   .name = SENSOR_NAME_2,
			   },
		.probe = sensor_probe,
		.remove = sensor_remove,
		.id_table = sensor_id_2,
	},
};

static __init int init_sensor(void)
{
	int i, ret = 0;

	sensor_dev_id = 0;
	for (i = 0; i < SENSOR_NUM; i++)
		ret = cci_dev_init_helper(&sensor_driver[i]);

	return ret;
}

static __exit void exit_sensor(void)
{
	int i;

	sensor_dev_id = 0;
	for (i = 0; i < SENSOR_NUM; i++)
		cci_dev_exit_helper(&sensor_driver[i]);
}

VIN_INIT_DRIVERS(init_sensor);
module_exit(exit_sensor);
