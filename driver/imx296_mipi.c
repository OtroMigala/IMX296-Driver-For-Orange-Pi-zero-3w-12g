// SPDX-License-Identifier: GPL-2.0
/*
 * Driver VIN (Allwinner) para el sensor Sony IMX296 de la Raspberry Pi
 * Global Shutter Camera, en la Orange Pi Zero 3W (A733, sun60iw2).
 *
 * Modo único: 1456x1088 RAW10, 1 carril MIPI, 60 cuadros/s, reloj de 54 MHz
 * (oscilador propio de la cámara).
 *
 * Derivado de:
 *   - bsp/drivers/vin/modules/sensor/imx219.c (Allwinner, GPL-2.0-or-later):
 *     enlace con VIN, compatibilidad con el kernel 6.6 y las dos instancias.
 *   - bsp/drivers/vin/modules/sensor/imx290_mipi.c (Allwinner, GPL-2.0-or-later):
 *     convenciones Sony (standby en 0x3000, exposición por SHS).
 *   - drivers/media/i2c/imx296.c (Laurent Pinchart, GPL-2.0) y su versión de
 *     Raspberry Pi (rama rpi-6.6.y): todos los valores de registros y el modo
 *     de disparo externo.
 *
 * Instalación, uso y limitaciones conocidas de VIN: README.md.
 *
 * Versiones:
 *   v1 (2026-10-17): primera captura, 10 cuadros 1456x1088 a 60 fps en CAM2.
 *   v2 (2026-10-17): parámetros rotation (180 por defecto, como Raspberry Pi),
 *       exposure_us y gain_db10; la exposición y la ganancia vigentes se aplican
 *       después de la tabla al arrancar la captura en lugar de quedar fijas en ella.
 *
 * Uso con VIN (ver scripts/04_fase4_pasos.sh): cargar vin_io, este módulo y luego
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
#include <media/v4l2-mediabus.h>
#include <linux/io.h>
#include "camera.h"
#include "sensor_helper.h"

MODULE_AUTHOR("Proyecto Ticanor");
MODULE_DESCRIPTION("VIN driver for the Sony IMX296 (Raspberry Pi Global Shutter Camera)");
MODULE_LICENSE("GPL");

/* 0 = libre (normal), 1 = disparo externo por XTR (fase 6). Igual que el driver de Raspberry Pi. */
static int trigger_mode;
module_param(trigger_mode, int, 0644);
MODULE_PARM_DESC(trigger_mode, "Set trigger mode: 0=default, 1=XTRIG");

/*
 * Orientación: el sensor va montado invertido en la cámara GS (imx296-overlay.dts de
 * Raspberry Pi declara rotation = <180>). 180 = espejo H+V en el sensor (CTRL0E = 0x03),
 * 0 = sin espejo. En la variante color (LQ) el espejo cambia el orden Bayer (RGGB -> BGGR);
 * las cámaras del proyecto son mono (LL), así que no afecta.
 */
static int rotation = 180;
module_param(rotation, int, 0644);
MODULE_PARM_DESC(rotation, "Image rotation applied in the sensor: 180 (default, as Raspberry Pi) or 0");

/*
 * Exposición y ganancia iniciales. Con isp_used = 1, VIN manda los controles de exposición
 * y ganancia al ISP (no al sensor), así que sin el servidor de ISP de Allwinner esta es la
 * forma de fijarlas. Se toman en cada S_INPUT/apertura y se aplican al arrancar la captura;
 * se pueden cambiar entre capturas en /sys/module/imx296_mipi/parameters/.
 */
static int exposure_us = 10000;
module_param(exposure_us, int, 0644);
MODULE_PARM_DESC(exposure_us, "Initial exposure in microseconds (default 10000; max ~16650 at 60 fps)");

static int gain_db10;
module_param(gain_db10, int, 0644);
MODULE_PARM_DESC(gain_db10, "Initial analogue gain in 0.1 dB steps, 0..480 (default 0)");

#define V4L2_IDENT_SENSOR	0x0296

/* 0x1a en 7 bits; VIN la guarda en 8 bits y hace >> 1 (vin.c, __vin_register_module) */
#define I2C_ADDR		0x34
#define SENSOR_NUM		0x2
#define SENSOR_NAME		"imx296_mipi"
#define SENSOR_NAME_2		"imx296_mipi_2"

/* Registros (nombres del driver del kernel principal). Multibyte: byte bajo en la dirección menor. */
#define IMX296_CTRL00		0x3000	/* bit 0: STANDBY */
#define IMX296_CTRL08		0x3008	/* bit 0: REGHOLD */
#define IMX296_CTRL0A		0x300a	/* bit 0: XMSTA (1 = transmisión detenida) */
#define IMX296_CTRL0B		0x300b	/* bit 0: TRIGEN */
#define IMX296_CTRL0E		0x300e	/* bit 0: VREVERSE, bit 1: HREVERSE */
#define IMX296_SHS1		0x308d	/* 24 bits */
#define IMX296_LOWLAGTRG	0x30ae	/* bit 0: FAST */
#define IMX296_SENSOR_INFO	0x3148	/* 16 bits, solo legible fuera de standby */
#define IMX296_GAIN		0x3204	/* 16 bits, 0..480 = 0..48 dB en pasos de 0,1 dB */

#define IMX296_SENSOR_INFO_MONO	0x8000

#define IMX296_WIDTH		1456
#define IMX296_HEIGHT		1088
/*
 * HMAX = 1100 ciclos de un reloj interno de 74,25 MHz = 14,815 us por línea.
 * En píxeles a la tasa nominal de 118,8 MHz (1188 Mbps / 10 bits): 1100 * 118,8 / 74,25 = 1760.
 */
#define IMX296_HTS		1760
#define IMX296_PCLK		118800000
#define IMX296_MIPI_BPS		1188000000
/* 118,8 MHz / (1760 * 1125) = 60,00 cuadros/s (medido en la Fase 4: 16,66 ms por cuadro) */
#define IMX296_VTS_60FPS	1125
/* Tiempo de línea: 1100 / 74,25 MHz = 14 814,8 ns */
#define IMX296_LINE_NS		14815
#define IMX296_GAIN_REG_MAX	480
/* Exposición inicial fija: 675 líneas = 10 ms -> SHS1 = 1125 - 675 = 450 = 0x0001c2 */
#define IMX296_EXP_INIT		675

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
	1010,1021,1033,1045,1057,1069,1082,1094,1107,1120,1133,1146,
	1159,1173,1186,1200,1214,1228,1242,1256,1271,1286,1301,1316,
	1331,1346,1362,1378,1394,1410,1426,1443,1459,1476,1493,1510,
	1528,1546,1564,1582,1600,1619,1637,1656,1675,1695,1714,1734,
	1754,1775,1795,1816,1837,1858,1880,1902,1924,1946,1968,1991,
	2014,2038,2061,2085,2109,2134,2158,2183,2209,2234,2260,2286,
	2313,2339,2367,2394,2422,2450,2478,2507,2536,2565,2595,2625,
	2655,2686,2717,2749,2780,2813,2845,2878,2912,2945,2979,3014,
	3049,3084,3120,3156,3192,3229,3267,3305,3343,3382,3421,3460,
	3500,3541,3582,3623,3665,3708,3751,3794,3838,3883,3928,3973,
	4019,
};
#define IMX296_GAIN_Q4_MAX	4019

static struct regval_list sensor_default_regs[] = {
};

/*
 * 1456x1088 RAW10, 1 carril, 60 cuadros/s, INCK = 54 MHz.
 * Se escribe con el sensor en standby. sensor_write_array() agrupa direcciones
 * consecutivas en una sola ráfaga I2C.
 */
static struct regval_list sensor_1456x1088_60fps_regs[] = {
	/* imx296_init_table: datos del fabricante sin documentar. La primera escritura activa CSI-2. */
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

	/* imx296_setup(): ventana completa, sin binning ni espejo */
	{0x3300, 0x00},		/* FID0_ROI: sin recorte */
	{0x4182, 0x40},		/* MIPIC_AREA3W = 1088 (solo Raspberry Pi) */
	{0x4183, 0x04},
	{0x300d, 0x00},		/* CTRL0D: WINMODE_ALL */
	/* CTRL0E (espejo) se escribe en sensor_reg_init() según el parámetro rotation */
	{0x3010, 0x65},		/* VMAX = 1125 */
	{0x3011, 0x04},
	{0x3012, 0x00},
	{0x3014, 0x4c},		/* HMAX = 1100 */
	{0x3015, 0x04},
	{0x3089, 0xb0},		/* INCKSEL0..3 para 54 MHz */
	{0x308a, 0x0f},
	{0x308b, 0xb0},
	{0x308c, 0x0c},
	/* SHS1 (exposición) y GAIN se escriben en sensor_reg_init() con los valores vigentes */
	{0x3212, 0x09},		/* GAINDLY = 1 cuadro (Raspberry Pi): ganancia y SHS1 en el mismo cuadro */
	{0x3254, 0x3c},		/* BLKLEVEL = 60 */
	{0x3255, 0x00},
	{0x4114, 0xc5},		/* GTTABLENUM */
	{0x418c, 0xa8},		/* CTRL418C = 168 para 54 MHz */
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

static unsigned int imx296_vts(struct sensor_info *info)
{
	return info->current_wins ? info->current_wins->vts : IMX296_VTS_60FPS;
}

/*
 * Toma exposure_us y gain_db10 (parámetros del módulo) y los deja en info->exp
 * (líneas * 16) e info->gain (Q4), las unidades de VIN. Solo memoria, sin I2C:
 * los registros se escriben al arrancar la captura (sensor_reg_init).
 */
static void imx296_apply_params(struct sensor_info *info)
{
	unsigned int vts = imx296_vts(info);
	u64 lines = ((u64)max(exposure_us, 1) * 1000 + IMX296_LINE_NS / 2) / IMX296_LINE_NS;
	int g = clamp(gain_db10, 0, IMX296_GAIN_REG_MAX);

	lines = clamp_t(u64, lines, 1, vts - 1);
	info->exp = (unsigned int)lines << 4;
	info->gain = imx296_gain_q4[g];
}

static int sensor_g_exp(struct v4l2_subdev *sd, __s32 *value)
{
	struct sensor_info *info = to_state(sd);

	*value = info->exp;
	return 0;
}

/* exp_val en unidades VIN: líneas * 16. Exposición = VMAX - SHS1. */
static int sensor_s_exp(struct v4l2_subdev *sd, unsigned int exp_val)
{
	struct sensor_info *info = to_state(sd);
	unsigned int vts = imx296_vts(info);
	unsigned int lines = exp_val >> 4;
	int ret;

	lines = clamp_t(unsigned int, lines, 1, vts - 1);
	ret = imx296_write_multi(sd, IMX296_SHS1, vts - lines, 3);
	if (ret)
		return ret;

	info->exp = lines << 4;
	sensor_dbg("%s: %u lineas, SHS1 = %u\n", __func__, lines, vts - lines);
	return 0;
}

static int sensor_g_gain(struct v4l2_subdev *sd, __s32 *value)
{
	struct sensor_info *info = to_state(sd);

	*value = info->gain;
	return 0;
}

/* gain_val en Q4 (16 = 1x). Se elige el primer paso de 0,1 dB que alcanza esa ganancia. */
static int sensor_s_gain(struct v4l2_subdev *sd, int gain_val)
{
	struct sensor_info *info = to_state(sd);
	int lo = 0, hi = IMX296_GAIN_REG_MAX, mid, ret;

	gain_val = clamp_t(int, gain_val, 16, IMX296_GAIN_Q4_MAX);
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (imx296_gain_q4[mid] < gain_val)
			lo = mid + 1;
		else
			hi = mid;
	}

	ret = imx296_write_multi(sd, IMX296_GAIN, lo, 2);
	if (ret)
		return ret;

	info->gain = gain_val;
	sensor_dbg("%s: Q4 %d -> GAIN %d (0,1 dB)\n", __func__, gain_val, lo);
	return 0;
}

/*
 * Exposición y ganancia por cuadro (3A de VIN). REGHOLD hace que ambos cambios
 * entren juntos; con GAINDLY = 1 cuadro, la ganancia coincide con el SHS1.
 */
static int sensor_s_exp_gain(struct v4l2_subdev *sd,
			     struct sensor_exp_gain *exp_gain)
{
	int ret;

	sensor_write(sd, IMX296_CTRL08, 0x01);
	ret = sensor_s_exp(sd, exp_gain->exp_val);
	if (!ret)
		ret = sensor_s_gain(sd, exp_gain->gain_val);
	sensor_write(sd, IMX296_CTRL08, 0x00);

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
		/* La salida de standby se hace en sensor_s_stream(), después de cargar la tabla. */
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
 * modelo ((valor >> 6) & 0x1ff == 296), como imx296_identify_model().
 */
static int sensor_detect(struct v4l2_subdev *sd)
{
	data_type lo = 0, hi = 0;
	unsigned int info, model;
	int ret;

	ret = sensor_write(sd, IMX296_CTRL00, 0x00);
	if (ret) {
		sensor_err("no responde al salir de standby (%d)\n", ret);
		return -ENODEV;
	}
	usleep_range(2000, 5000);

	ret = sensor_read(sd, IMX296_SENSOR_INFO, &lo);
	if (!ret)
		ret = sensor_read(sd, IMX296_SENSOR_INFO + 1, &hi);
	sensor_write(sd, IMX296_CTRL00, 0x01);
	if (ret) {
		sensor_err("no se pudo leer SENSOR_INFO (%d)\n", ret);
		return -ENODEV;
	}

	info = ((hi & 0xff) << 8) | (lo & 0xff);
	model = (info >> 6) & 0x1ff;
	if (model != 296) {
		sensor_err("modelo invalido: SENSOR_INFO = 0x%04x\n", info);
		return -ENODEV;
	}

	sensor_print("Sony IMX296%s detectada (SENSOR_INFO = 0x%04x)\n",
		     (info & IMX296_SENSOR_INFO_MONO) ? "LL (mono)" : "LQ (color)", info);
	return 0;
}

static int sensor_init(struct v4l2_subdev *sd, u32 val)
{
	struct sensor_info *info = to_state(sd);
	int ret;

	ret = sensor_detect(sd);
	if (ret) {
		sensor_err("chip found is not an target chip.\n");
		return ret;
	}

	info->focus_status = 0;
	info->low_speed = 0;
	info->width = IMX296_WIDTH;
	info->height = IMX296_HEIGHT;
	info->hflip = 0;
	info->vflip = 0;
	imx296_apply_params(info);
	info->tpf.numerator = 1;
	info->tpf.denominator = 60;
	info->preview_first_flag = 1;

	return 0;
}

static long sensor_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct sensor_info *info = to_state(sd);
	int ret = 0;

	switch (cmd) {
	case GET_CURRENT_WIN_CFG:
		if (info->current_wins != NULL) {
			memcpy(arg, info->current_wins, sizeof(*info->current_wins));
		} else {
			sensor_err("empty wins!\n");
			ret = -1;
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
 * Bayer RGGB sin espejo, según la tabla de formatos del driver de Raspberry Pi
 * (el kernel principal dice BGGR): hay que confirmarlo con una imagen real.
 * VIN no reconoce MEDIA_BUS_FMT_Y10_1X10, así que la variante mono usaría el mismo código.
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
		.intg_max = (IMX296_VTS_60FPS - 1) << 4,
		.gain_min = 1 << 4,
		.gain_max = IMX296_GAIN_Q4_MAX,
		.regs = sensor_1456x1088_60fps_regs,
		.regs_size = ARRAY_SIZE(sensor_1456x1088_60fps_regs),
		.set_size = NULL,
	},
};
#define N_WIN_SIZES (ARRAY_SIZE(sensor_win_sizes))

static int sensor_reg_init(struct sensor_info *info)
{
	struct v4l2_subdev *sd = &info->sd;
	struct sensor_format_struct *sensor_fmt = info->fmt;
	struct sensor_win_size *wsize = info->current_wins;
	int ret;

	if (!wsize || !sensor_fmt) {
		sensor_err("formato sin inicializar\n");
		return -EINVAL;
	}

	ret = sensor_write_array(sd, sensor_default_regs,
				 ARRAY_SIZE(sensor_default_regs));
	if (ret < 0) {
		sensor_err("write sensor_default_regs error\n");
		return ret;
	}

	sensor_write_array(sd, sensor_fmt->regs, sensor_fmt->regs_size);

	if (wsize->regs) {
		ret = sensor_write_array(sd, wsize->regs, wsize->regs_size);
		if (ret < 0) {
			sensor_err("write mode regs error\n");
			return ret;
		}
	}

	if (wsize->set_size)
		wsize->set_size(sd);

	/*
	 * Orientación, exposición y ganancia vigentes. No van en la tabla para no pisar
	 * lo que se haya fijado antes de arrancar (parámetros, controles del subdev o 3A).
	 * El sensor sigue en standby, así que los cambios entran desde el primer cuadro.
	 */
	ret = sensor_write(sd, IMX296_CTRL0E, rotation == 180 ? 0x03 : 0x00);
	if (!ret)
		ret = sensor_s_exp(sd, info->exp);
	if (!ret)
		ret = sensor_s_gain(sd, info->gain);
	if (ret) {
		sensor_err("no se pudo fijar orientación/exposición/ganancia (%d)\n", ret);
		return ret;
	}

	info->width = wsize->width;
	info->height = wsize->height;

	sensor_print("s_fmt %dx%d, rotacion %d, exposicion %u lineas (%u us), ganancia Q4 %u\n",
		     wsize->width, wsize->height, rotation == 180 ? 180 : 0, info->exp >> 4,
		     (info->exp >> 4) * IMX296_LINE_NS / 1000, info->gain);
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
	ret |= sensor_write(sd, IMX296_CTRL0B, trigger_mode == 1 ? 0x01 : 0x00);
	ret |= sensor_write(sd, IMX296_LOWLAGTRG, trigger_mode == 1 ? 0x01 : 0x00);
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
		sensor_err("no se pudo detener la transmisión (%d)\n", ret);

	return 0;
}

static int sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sensor_info *info = to_state(sd);
	int ret;

	if (!info->current_wins || !info->fmt) {
		sensor_err("%s: formato sin inicializar\n", __func__);
		return enable ? -EINVAL : 0;
	}

	sensor_print("%s on = %d, %d*%d %x, trigger_mode = %d\n", __func__, enable,
		     info->current_wins->width, info->current_wins->height,
		     info->fmt->mbus_code, trigger_mode);

	if (!enable)
		return sensor_stop(sd);

	ret = sensor_reg_init(info);
	if (ret)
		return ret;

	return sensor_start(sd);
}

/* VIN usa num_data_lanes como máscara de flags (platform_cfg.h): 1 carril, canal virtual 0. */
static int sensor_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_mbus_config *cfg)
{
	cfg->type = V4L2_MBUS_CSI2_DPHY;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	cfg->bus.mipi_csi2.num_data_lanes = 0 | V4L2_MBUS_CSI2_1_LANE | V4L2_MBUS_CSI2_CHANNEL_0;
#else
	cfg->flags = 0 | V4L2_MBUS_CSI2_1_LANE | V4L2_MBUS_CSI2_CHANNEL_0;
#endif
	return 0;
}

static int sensor_g_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor_info *info =
			container_of(ctrl->handler, struct sensor_info, handler);
	struct v4l2_subdev *sd = &info->sd;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		return sensor_g_gain(sd, &ctrl->val);
	case V4L2_CID_EXPOSURE:
		return sensor_g_exp(sd, &ctrl->val);
	}
	return -EINVAL;
}

static int sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor_info *info =
			container_of(ctrl->handler, struct sensor_info, handler);
	struct v4l2_subdev *sd = &info->sd;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		return sensor_s_gain(sd, ctrl->val);
	case V4L2_CID_EXPOSURE:
		return sensor_s_exp(sd, ctrl->val);
	}
	return -EINVAL;
}

/* ----------------------------------------------------------------------- */

static const struct v4l2_ctrl_ops sensor_ctrl_ops = {
	.g_volatile_ctrl = sensor_g_ctrl,
	.s_ctrl = sensor_s_ctrl,
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
	.set_fmt = sensor_set_fmt,
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

/* Exposición en Q4 (líneas * 16) y ganancia en Q4 (16 = 1x), las unidades de VIN. */
static int sensor_init_controls(struct v4l2_subdev *sd, const struct v4l2_ctrl_ops *ops)
{
	struct sensor_info *info = to_state(sd);
	struct v4l2_ctrl_handler *handler = &info->handler;
	struct v4l2_ctrl *ctrl;
	int ret = 0;

	v4l2_ctrl_handler_init(handler, 2);

	ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_EXPOSURE, 1 << 4,
				 (IMX296_VTS_60FPS - 1) << 4, 1, IMX296_EXP_INIT << 4);
	if (ctrl != NULL)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_GAIN, 16,
				 IMX296_GAIN_Q4_MAX, 1, 16);
	if (ctrl != NULL)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

	if (handler->error) {
		ret = handler->error;
		v4l2_ctrl_handler_free(handler);
	}

	sd->ctrl_handler = handler;
	return ret;
}

/* Solo se usa en el modo CCI de VIN (CONFIG_CSI_CCI), desactivado en la A733. */
static int sensor_dev_id;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static int sensor_probe(struct i2c_client *client)
#else
static int sensor_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#endif
{
	struct v4l2_subdev *sd;
	struct sensor_info *info;
	int i;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (info == NULL)
		return -ENOMEM;
	sd = &info->sd;
	/* Antes que nada: sensor_registered() (cci_helper.c) toma info->lock. */
	mutex_init(&info->lock);

	if (client) {
		for (i = 0; i < SENSOR_NUM; i++) {
			if (!strcmp(cci_drv[i].name, client->name))
				break;
		}
		cci_dev_probe_helper(sd, client, &sensor_ops, &cci_drv[i]);
	} else {
		cci_dev_probe_helper(sd, client, &sensor_ops, &cci_drv[sensor_dev_id++]);
	}

	sensor_init_controls(sd, &sensor_ctrl_ops);

#if IS_ENABLED(CONFIG_SAME_I2C)
	info->sensor_i2c_addr = I2C_ADDR >> 1;
#endif
	info->fmt = &sensor_formats[0];
	info->fmt_pt = &sensor_formats[0];
	info->win_pt = &sensor_win_sizes[0];
	/*
	 * Hay un solo modo: se fija desde ya para que ningún camino (get_fmt sobre
	 * /dev/v4l-subdevN, sensor_get_exp, sensor_get_clk, s_stream) encuentre NULL
	 * antes del primer VIDIOC_S_FMT. sensor_set_fmt() lo vuelve a fijar igual.
	 */
	info->current_wins = &sensor_win_sizes[0];
	info->fmt_num = N_FMTS;
	info->win_size_num = N_WIN_SIZES;
	info->sensor_field = V4L2_FIELD_NONE;
	info->af_first_flag = 1;
	imx296_apply_params(info);

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
static int sensor_remove(struct i2c_client *client)
#else
static void sensor_remove(struct i2c_client *client)
#endif
{
	struct v4l2_subdev *sd;
	int i;

	if (client) {
		for (i = 0; i < SENSOR_NUM; i++) {
			if (!strcmp(cci_drv[i].name, client->name))
				break;
		}
		sd = cci_dev_remove_helper(client, &cci_drv[i]);
	} else {
		sd = cci_dev_remove_helper(client, &cci_drv[sensor_dev_id++]);
	}
	/* cci_dev_remove_helper() ya desregistró el subdev; los controles se pueden liberar. */
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	kfree(to_state(sd));
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#endif
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
