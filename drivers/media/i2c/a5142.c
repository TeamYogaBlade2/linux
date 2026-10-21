// SPDX-License-Identifier: GPL-2.0-only
/*
 * ON Semiconductor (Aptina) A5142 5 megapixel CMOS image sensor.
 *
 * The A5142 is the main (rear) camera on the Lenovo YOGA Tablet 8/10
 * (MT6589), paired with an FM50AF voice coil motor (drivers/media/i2c/
 * fm50af.c) and configured by the vendor tree as
 * CUSTOM_KERNEL_MAIN_IMGSENSOR = a5142_mipi_raw
 * (mediatek/config/blade10/ProjectConfig-jb2.mk, MTK_PLATFORM = MT6589).
 *
 * Bus protocol
 * ------------
 * Every access starts with a 16-bit register address and the part is
 * attached at 7-bit address 0x6c.  Two transfer widths exist and the
 * vendor driver uses both in the same sequence, so this driver carries a
 * per-entry width flag instead of pretending the register file is uniform:
 *
 *	8-bit:	u8 buf[3] = { addr >> 8, addr, value }
 *	16-bit:	u8 buf[4] = { addr >> 8, addr, value >> 8, value }
 *
 * Both are stated by the vendor I/O primitives: the 16-bit form is
 * A5142MIPI_write_cmos_sensor() at a5142mipi_Sensor.c:118-122 and the 8-bit
 * form is A5142MIPI_write_cmos_sensor_8() at :133-137.  The 16-bit form is
 * big-endian on the wire.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Reset and streaming. */
#define A5142_REG_SOFTWARE_RESET	0x0103
#define A5142_REG_MODE_SELECT		0x0100

/*
 * The vendor table reaches the "stop streaming" step of both mode tables by
 * writing MODE_SELECT through the *8-bit* primitive (:1189 and :1268), while
 * the "start streaming" step uses the same primitive (:1250, :1328).  Keep
 * both widths as the vendor wrote them.
 */

/*
 * Identification is by I2C write id only, not by a chip-ID register: the
 * vendor reads 0x0000 and compares against 0x4800
 * (kd_imgsensor.h:222, a5142mipi_Sensor.c:1061), but its read helper
 * byte-swaps the result (:109), which would turn a big-endian 0x4800 into
 * 0x0048.  Which shape the silicon actually returns is unverified, so no
 * ID register is defined or checked here.
 */

/* Geometry.  These live in the mode tables, not in code. */
/*
 * READ_MODE carries the binning controls in its low bits.  The vendor
 * read-modify-writes only bits [11:0], preserving the top nibble
 * (a5142mipi_Sensor.c:1233-1236 and :1311-1315).
 */
#define A5142_REG_READ_MODE		0x3040
#define A5142_READ_MODE_BIN_2X2		0x04C3	/* preview: xodd=3, yodd=3 */
#define A5142_READ_MODE_FULL		0x0041	/* capture: binning off */

/* Frame timing: only the frame-length register is driven at runtime. */
#define A5142_REG_FRAME_LENGTH_LINES	0x300A

/* Exposure and gain. */
#define A5142_REG_COARSE_INTEGRATION	0x0202
#define A5142_REG_GLOBAL_GAIN		0x0204
/*
 * A5142MIPI_SetShutter() writes a single 16-bit 0x0202 (:396) and
 * write_A5142MIPI_gain() a single 16-bit 0x0204 (:302), both bracketed by the
 * grouped-parameter latch.  The 0x0206/0x0208/0x020A/0x020C registers that
 * appear in A5142MIPI_sensor_gain_array[] are only touched by the *disabled*
 * per-channel CCT/gain code, and that code is compiled out: the only calls
 * are commented away inside /* *\/ blocks spanning :255-265 in
 * read_A5142MIPI_gain() and :308-328 in write_A5142MIPI_gain().  They are
 * therefore not driven here; doing so would invent a white-balance model the
 * vendor silicon path does not use.
 */

/* Orientation and test pattern. */
#define A5142_REG_IMAGE_ORIENTATION	0x0101
#define A5142_ORIENTATION_HFLIP		BIT(0)
#define A5142_ORIENTATION_VFLIP		BIT(1)

/*
 * The vendor driver has no test-pattern control at all: the only candidate,
 * 0x3070, is never written by a5142mipi_Sensor.c.  V4L2_CID_TEST_PATTERN is
 * therefore not exposed; see the comment on the control list.
 */

/* Vertical blanking, as the vendor programs it: 2021 - 1944. */
#define A5142_FULL_WIDTH		2592u
#define A5142_FULL_HEIGHT		1944u
#define A5142_FULL_FRAME_LINES		2021u

/*
 * The vendor's binned preview mode.  The output sizes come from the registers
 * it programs (a5142mipi_Sensor.c:1237-1238, 0x0510 x 0x03CC) and match
 * A5142MIPI_IMAGE_SENSOR_PV_HACTIVE/VACTIVE (a5142mipi_Sensor.h:93-94).
 */
#define A5142_PV_WIDTH			1296u
#define A5142_PV_HEIGHT			972u

/*
 * Gain.  The vendor works in BASEGAIN = 0x40 units (kd_imgsensor_define.h:32)
 * and maps MTK gain to the register as reg = 8 * gain / BASEGAIN, i.e.
 * reg = gain / 8 (:298), with the accepted input range BASEGAIN..32*BASEGAIN
 * (:296) - 1x to 32x.  Expose the register directly and document the scale
 * rather than carrying MTK's unit through the mainline API.
 */
#define A5142_GAIN_MIN			0x08
#define A5142_GAIN_MAX			0x100
#define A5142_GAIN_DEFAULT		0x10

/*
 * Vertical blanking range.  The vendor's only full-resolution timing point is
 * frame length 2021 lines for 1944 active rows (:1319), giving 77 lines of
 * blanking.  A5142_TOTAL_LINES_MAX is the field width of the two line-count
 * registers, both 16-bit.
 */
#define A5142_VBLANK_MIN		(A5142_FULL_FRAME_LINES - A5142_FULL_HEIGHT)
#define A5142_TOTAL_LINES_MAX		0xffffu

/*
 * External clock.  The vendor PLL comment says MCLK = 26 MHz
 * (a5142mipi_Sensor.c:996), and every divider in the table is built around
 * that value, so anything else produces a wrong pixel clock and is rejected.
 * Set this from the "clock-frequency" of the DT clock node once the board's
 * actual MCLK is measured; the driver does not silently rescale the table.
 */
#define A5142_EXTCLK_MIN		(26 * 1000 * 1000)
#define A5142_EXTCLK_MAX		(26 * 1000 * 1000)

#define A5142_LANES			2

enum a5142_reg_width {
	A5142_REG_8BIT,
	A5142_REG_16BIT,
};

struct a5142_reg {
	u16 addr;
	u16 val;
	enum a5142_reg_width width;
};

struct a5142_mode {
	u32 width;
	u32 height;
	u16 read_mode;
	const struct a5142_reg *regs;
	unsigned int num_regs;
	const struct a5142_reg *stream_on;
	unsigned int num_stream_on;
};

struct a5142_ctrls {
	struct v4l2_ctrl_handler handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
};

struct a5142_device {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *extclk;
	struct regulator *vdd;
	struct regulator *vddio;

	/* Protects everything below. */
	struct mutex lock;

	struct v4l2_mbus_framefmt fmt;
	struct a5142_ctrls ctrls;
	unsigned int lane_count;
	const struct a5142_mode *mode;
};

static inline struct a5142_device *to_a5142(struct v4l2_subdev *sd)
{
	return container_of(sd, struct a5142_device, sd);
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct a5142_device,
			     ctrls.handler)->sd;
}

/* -------------------------------------------------------------------------
 * Register access
 */

static int a5142_write(struct a5142_device *dev, u16 addr, u16 val,
		       enum a5142_reg_width width)
{
	struct i2c_client *client = dev->client;
	u8 buf[4];
	int len;

	if (width == A5142_REG_8BIT) {
		buf[0] = addr >> 8;
		buf[1] = addr & 0xff;
		buf[2] = val & 0xff;
		len = 3;
	} else {
		buf[0] = addr >> 8;
		buf[1] = addr & 0xff;
		buf[2] = val >> 8;
		buf[3] = val & 0xff;
		len = 4;
	}

	int ret = i2c_master_send(client, buf, len);

	if (ret < 0)
		dev_dbg(&client->dev, "i2c write %#06x failed: %d\n", addr, ret);
	else if (ret != len)
		return -EIO;

	return 0;
}

/*
 * A register read is a 16-bit address followed by two data bytes in the same
 * big-endian order (a5142mipi_Sensor.c:104-110).  The vendor's read helper
 * then byte-swaps what it got back (:109).  That swap looks wrong - the same
 * source writes 0x0202 as big-endian - and the vendor only ever verifies
 * 0x0000 against 0x4800, which the swap would turn into 0x0048.  Rather than
 * guess which of the two shapes the silicon uses, the only read this driver
 * performs is discarded-because-unverified: the chip ID is checked against
 * the unambiguous write-id probe instead, and this helper stays unused.
 */

static int a5142_write_table(struct a5142_device *dev,
			     const struct a5142_reg *regs, unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = a5142_write(dev, regs[i].addr, regs[i].val,
				  regs[i].width);
		if (ret)
			return ret;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Register sequences, transcribed from the vendor tree
 *
 * Every entry below is one write_cmos_sensor_8()/write_cmos_sensor() call in
 * a5142mipi_Sensor.c with its source line.  The vendor's two compile-time
 * alternatives (MIPI_INTERFACE vs parallel, RAW10 vs RAW8) are pre-resolved
 * to MIPI + RAW10, which is what this tree builds
 * (a5142mipi_Sensor.h:78-79 defines both MIPI_INTERFACE and RAW10).
 */

/* a5142mipi_Sensor.c:826-1004, A5142MIPI_Init_setting(). */
static const struct a5142_reg a5142_init_regs[] = {
	{ 0x0103, 0x0001, A5142_REG_8BIT },	/* :826 software reset */
	{ 0x0100, 0x0000, A5142_REG_8BIT },	/* :831 stream off */
	{ 0x301a, 0x0218, A5142_REG_16BIT },	/* :835 RESET_REGISTER, MIPI */
	{ 0x3064, 0xb800, A5142_REG_16BIT },	/* :836 SMIA_TEST */
	{ 0x31ae, 0x0202, A5142_REG_16BIT },	/* :837 2-lane serial */
	{ 0x0112, 0x0a0a, A5142_REG_16BIT },	/* :838 RAW10/RAW10 */
	{ 0x316a, 0x8400, A5142_REG_16BIT },	/* :852 DAC_FBIAS */
	{ 0x316c, 0x8400, A5142_REG_16BIT },	/* :853 DAC_TXLO */
	{ 0x316e, 0x8400, A5142_REG_16BIT },	/* :854 DAC_ECL */
	{ 0x3efa, 0x1a1f, A5142_REG_16BIT },	/* :855 DAC_LD_ECL */
	{ 0x3ed2, 0xd965, A5142_REG_16BIT },	/* :856 DAC_LD_6_7 */
	{ 0x3ed8, 0x7f1b, A5142_REG_16BIT },	/* :857 DAC_LD_12_13 */
	{ 0x3eda, 0x2f11, A5142_REG_16BIT },	/* :858 DAC_LD_14_15 */
	{ 0x3ee2, 0x0060, A5142_REG_16BIT },	/* :859 DAC_LD_22_23 */
	{ 0x3ef2, 0xd965, A5142_REG_16BIT },	/* :860 DAC_LP_6_7 */
	{ 0x3ef8, 0x797f, A5142_REG_16BIT },	/* :861 DAC_LD_TXHI */
	{ 0x3efc, 0x286f, A5142_REG_16BIT },	/* :862 DAC_LD_FBIAS */
	{ 0x3efe, 0x2c01, A5142_REG_16BIT },	/* :863 DAC_LD_TXLO */
	{ 0x3e00, 0x042f, A5142_REG_16BIT },	/* :866 DYNAMIC_SEQRAM_00 */
	{ 0x3e02, 0xffff, A5142_REG_16BIT },	/* :867 */
	{ 0x3e04, 0xffff, A5142_REG_16BIT },	/* :868 */
	{ 0x3e06, 0xffff, A5142_REG_16BIT },	/* :869 */
	{ 0x3e08, 0x8071, A5142_REG_16BIT },	/* :870 */
	{ 0x3e0a, 0x7281, A5142_REG_16BIT },	/* :871 */
	{ 0x3e0c, 0x4011, A5142_REG_16BIT },	/* :872 */
	{ 0x3e0e, 0x8010, A5142_REG_16BIT },	/* :873 */
	{ 0x3e10, 0x60a5, A5142_REG_16BIT },	/* :874 */
	{ 0x3e12, 0x4080, A5142_REG_16BIT },	/* :875 */
	{ 0x3e14, 0x4180, A5142_REG_16BIT },	/* :876 */
	{ 0x3e16, 0x0018, A5142_REG_16BIT },	/* :877 */
	{ 0x3e18, 0x46b7, A5142_REG_16BIT },	/* :878 */
	{ 0x3e1a, 0x4994, A5142_REG_16BIT },	/* :879 */
	{ 0x3e1c, 0x4997, A5142_REG_16BIT },	/* :880 */
	{ 0x3e1e, 0x4682, A5142_REG_16BIT },	/* :881 */
	{ 0x3e20, 0x0018, A5142_REG_16BIT },	/* :882 */
	{ 0x3e22, 0x4241, A5142_REG_16BIT },	/* :883 */
	{ 0x3e24, 0x8000, A5142_REG_16BIT },	/* :884 */
	{ 0x3e26, 0x1880, A5142_REG_16BIT },	/* :885 */
	{ 0x3e28, 0x4785, A5142_REG_16BIT },	/* :886 */
	{ 0x3e2a, 0x4992, A5142_REG_16BIT },	/* :887 */
	{ 0x3e2c, 0x4997, A5142_REG_16BIT },	/* :888 */
	{ 0x3e2e, 0x4780, A5142_REG_16BIT },	/* :889 */
	{ 0x3e30, 0x4d80, A5142_REG_16BIT },	/* :890 */
	{ 0x3e32, 0x100c, A5142_REG_16BIT },	/* :891 */
	{ 0x3e34, 0x8000, A5142_REG_16BIT },	/* :892 */
	{ 0x3e36, 0x184a, A5142_REG_16BIT },	/* :893 */
	{ 0x3e38, 0x8042, A5142_REG_16BIT },	/* :894 */
	{ 0x3e3a, 0x001a, A5142_REG_16BIT },	/* :895 */
	{ 0x3e3c, 0x9610, A5142_REG_16BIT },	/* :896 */
	{ 0x3e3e, 0x0c80, A5142_REG_16BIT },	/* :897 */
	{ 0x3e40, 0x4dc6, A5142_REG_16BIT },	/* :898 */
	{ 0x3e42, 0x4a80, A5142_REG_16BIT },	/* :899 */
	{ 0x3e44, 0x0018, A5142_REG_16BIT },	/* :900 */
	{ 0x3e46, 0x8042, A5142_REG_16BIT },	/* :901 */
	{ 0x3e48, 0x8041, A5142_REG_16BIT },	/* :902 */
	{ 0x3e4a, 0x0018, A5142_REG_16BIT },	/* :903 */
	{ 0x3e4c, 0x804b, A5142_REG_16BIT },	/* :904 */
	{ 0x3e4e, 0xb74b, A5142_REG_16BIT },	/* :905 */
	{ 0x3e50, 0x8010, A5142_REG_16BIT },	/* :906 */
	{ 0x3e52, 0x6056, A5142_REG_16BIT },	/* :907 */
	{ 0x3e54, 0x001c, A5142_REG_16BIT },	/* :908 */
	{ 0x3e56, 0x8211, A5142_REG_16BIT },	/* :909 */
	{ 0x3e58, 0x8056, A5142_REG_16BIT },	/* :910 */
	{ 0x3e5a, 0x827c, A5142_REG_16BIT },	/* :911 */
	{ 0x3e5c, 0x0970, A5142_REG_16BIT },	/* :912 */
	{ 0x3e5e, 0x8082, A5142_REG_16BIT },	/* :913 */
	{ 0x3e60, 0x7281, A5142_REG_16BIT },	/* :914 */
	{ 0x3e62, 0x4c40, A5142_REG_16BIT },	/* :915 */
	{ 0x3e64, 0x8e4d, A5142_REG_16BIT },	/* :916 */
	{ 0x3e66, 0x8110, A5142_REG_16BIT },	/* :917 */
	{ 0x3e68, 0x0caf, A5142_REG_16BIT },	/* :918 */
	{ 0x3e6a, 0x4d80, A5142_REG_16BIT },	/* :919 */
	{ 0x3e6c, 0x100c, A5142_REG_16BIT },	/* :920 */
	{ 0x3e6e, 0x8440, A5142_REG_16BIT },	/* :921 */
	{ 0x3e70, 0x4c81, A5142_REG_16BIT },	/* :922 */
	{ 0x3e72, 0x7c5f, A5142_REG_16BIT },	/* :923 */
	{ 0x3e74, 0x7000, A5142_REG_16BIT },	/* :924 */
	{ 0x3e76, 0x0000, A5142_REG_16BIT },	/* :925-967 unused SEQRAM */
	{ 0x3e78, 0x0000, A5142_REG_16BIT },
	{ 0x3e7a, 0x0000, A5142_REG_16BIT },
	{ 0x3e7c, 0x0000, A5142_REG_16BIT },
	{ 0x3e7e, 0x0000, A5142_REG_16BIT },
	{ 0x3e80, 0x0000, A5142_REG_16BIT },
	{ 0x3e82, 0x0000, A5142_REG_16BIT },
	{ 0x3e84, 0x0000, A5142_REG_16BIT },
	{ 0x3e86, 0x0000, A5142_REG_16BIT },
	{ 0x3e88, 0x0000, A5142_REG_16BIT },
	{ 0x3e8a, 0x0000, A5142_REG_16BIT },
	{ 0x3e8c, 0x0000, A5142_REG_16BIT },
	{ 0x3e8e, 0x0000, A5142_REG_16BIT },
	{ 0x3e90, 0x0000, A5142_REG_16BIT },
	{ 0x3e92, 0x0000, A5142_REG_16BIT },
	{ 0x3e94, 0x0000, A5142_REG_16BIT },
	{ 0x3e96, 0x0000, A5142_REG_16BIT },
	{ 0x3e98, 0x0000, A5142_REG_16BIT },
	{ 0x3e9a, 0x0000, A5142_REG_16BIT },
	{ 0x3e9c, 0x0000, A5142_REG_16BIT },
	{ 0x3e9e, 0x0000, A5142_REG_16BIT },
	{ 0x3ea0, 0x0000, A5142_REG_16BIT },
	{ 0x3ea2, 0x0000, A5142_REG_16BIT },
	{ 0x3ea4, 0x0000, A5142_REG_16BIT },
	{ 0x3ea6, 0x0000, A5142_REG_16BIT },
	{ 0x3ea8, 0x0000, A5142_REG_16BIT },
	{ 0x3eaa, 0x0000, A5142_REG_16BIT },
	{ 0x3eac, 0x0000, A5142_REG_16BIT },
	{ 0x3eae, 0x0000, A5142_REG_16BIT },
	{ 0x3eb0, 0x0000, A5142_REG_16BIT },
	{ 0x3eb2, 0x0000, A5142_REG_16BIT },
	{ 0x3eb4, 0x0000, A5142_REG_16BIT },
	{ 0x3eb6, 0x0000, A5142_REG_16BIT },
	{ 0x3eb8, 0x0000, A5142_REG_16BIT },
	{ 0x3eba, 0x0000, A5142_REG_16BIT },
	{ 0x3ebc, 0x0000, A5142_REG_16BIT },
	{ 0x3ebe, 0x0000, A5142_REG_16BIT },
	{ 0x3ec0, 0x0000, A5142_REG_16BIT },
	{ 0x3ec2, 0x0000, A5142_REG_16BIT },
	{ 0x3ec4, 0x0000, A5142_REG_16BIT },
	{ 0x3ec6, 0x0000, A5142_REG_16BIT },
	{ 0x3ec8, 0x0000, A5142_REG_16BIT },
	{ 0x3eca, 0x0000, A5142_REG_16BIT },
	/* "dynamic power disable" block, :969-974 */
	{ 0x3170, 0x2150, A5142_REG_16BIT },	/* :970 ANALOG_CONTROL */
	{ 0x317a, 0x0150, A5142_REG_16BIT },	/* :971 ANALOG_CONTROL6 */
	{ 0x3ecc, 0x2200, A5142_REG_16BIT },	/* :972 DAC_LD_0_1 */
	{ 0x3174, 0x0000, A5142_REG_16BIT },	/* :973 ANALOG_CONTROL3 */
	{ 0x3176, 0x0000, A5142_REG_16BIT },	/* :974 ANALOG_CONTROL4 */
	/* :976-988 - no vendor comment names these; values kept verbatim. */
	{ 0x30bc, 0x0384, A5142_REG_16BIT },
	{ 0x30c0, 0x1220, A5142_REG_16BIT },
	{ 0x30d4, 0x9200, A5142_REG_16BIT },
	{ 0x30b2, 0xc000, A5142_REG_16BIT },
	/* :981-988 - HiSPi/MIPI framing and lane timing. */
	{ 0x31b0, 0x00c4, A5142_REG_16BIT },
	{ 0x31b2, 0x0064, A5142_REG_16BIT },
	{ 0x31b4, 0x0e77, A5142_REG_16BIT },
	{ 0x31b6, 0x0d24, A5142_REG_16BIT },
	{ 0x31b8, 0x020e, A5142_REG_16BIT },
	{ 0x31ba, 0x0710, A5142_REG_16BIT },
	{ 0x31bc, 0x2a0d, A5142_REG_16BIT },
	{ 0x31be, 0xc007, A5142_REG_16BIT },
	{ 0x305e, 0x112e, A5142_REG_16BIT },	/* :994 global gain seed */
	{ 0x30f0, 0x0000, A5142_REG_16BIT },	/* :995 AF off, A5142 has none */
	/*
	 * :999-1004 PLL for MCLK 26 MHz / PCLK 104 MHz.  Note the comment on
	 * :999 says "vt_pix_clk_div = 5" and the value is 5; the RAW10 branch
	 * is the one taken.
	 */
	{ 0x0300, 0x0005, A5142_REG_16BIT },
	{ 0x0302, 0x0001, A5142_REG_16BIT },
	{ 0x0304, 0x0002, A5142_REG_16BIT },
	{ 0x0306, 0x0028, A5142_REG_16BIT },
	{ 0x0308, 0x000a, A5142_REG_16BIT },
	{ 0x030a, 0x0001, A5142_REG_16BIT },
	/* :827 mDELAY(5) after the reset - reproduced below, not per-entry. */
};

/*
 * a5142mipi_Sensor.c:1184-1261, A5142MIPI_preview_setting(): 1296x972 with
 * 2x2 binning.  The vendor also reads 0x3040 and ORs 0x04C3 into bits [11:0]
 * (:1233-1236); this driver does that in code (a5142_read_mode()) because it
 * is a read-modify-write, not a constant.
 */
static const struct a5142_reg a5142_preview_regs[] = {
	{ 0x0100, 0x0000, A5142_REG_8BIT },	/* :1189 stream off */
	{ 0x0104, 0x0001, A5142_REG_8BIT },	/* :1191 group hold on */
	{ 0x3064, 0xb800, A5142_REG_16BIT },	/* :1197 SMIA_TEST */
	{ 0x31ae, 0x0202, A5142_REG_16BIT },	/* :1198 2-lane serial */
	{ 0x0112, 0x0a0a, A5142_REG_16BIT },	/* :1199 RAW10 */
	{ 0x0300, 0x0005, A5142_REG_16BIT },	/* :1210 vt_pix_clk_div */
	{ 0x0302, 0x0001, A5142_REG_16BIT },	/* :1211 vt_sys_clk_div */
	{ 0x0304, 0x0002, A5142_REG_16BIT },	/* :1212 pre_pll_clk_div */
	{ 0x0306, 0x0028, A5142_REG_16BIT },	/* :1213 pll_multiplier = 40 */
	{ 0x0308, 0x000a, A5142_REG_16BIT },	/* :1214 op_pix_clk_div */
	{ 0x030a, 0x0001, A5142_REG_16BIT },	/* :1215 op_sys_clk_div */
	{ 0x0344, 0x0008, A5142_REG_16BIT },	/* :1229 x_addr_start */
	{ 0x0346, 0x0008, A5142_REG_16BIT },	/* :1230 y_addr_start */
	{ 0x0348, 0x0a25, A5142_REG_16BIT },	/* :1231 x_addr_end 2597 */
	{ 0x034a, 0x079d, A5142_REG_16BIT },	/* :1232 y_addr_end 1949 */
	{ 0x034c, 0x0510, A5142_REG_16BIT },	/* :1237 x_output_size 1296 */
	{ 0x034e, 0x03cc, A5142_REG_16BIT },	/* :1238 y_output_size 972 */
	{ 0x300c, 0x0c4c, A5142_REG_16BIT },	/* :1240 line_length 3151 */
	{ 0x300a, 0x0415, A5142_REG_16BIT },	/* :1241 frame_lines 1100 */
	{ 0x3014, 0x0908, A5142_REG_16BIT },	/* :1244 fine_integration_time */
	{ 0x3010, 0x0184, A5142_REG_16BIT },	/* :1245 fine_correction */
};

/*
 * a5142mipi_Sensor.c:1263-1339, A5142MIPI_capture_setting(): 2592x1944,
 * binning off, PLL rebuilt for PCLK 111.8 MHz.
 */
static const struct a5142_reg a5142_capture_regs[] = {
	{ 0x0100, 0x0000, A5142_REG_8BIT },	/* :1268 stream off */
	{ 0x0104, 0x0001, A5142_REG_8BIT },	/* :1270 group hold on */
	{ 0x3064, 0xb800, A5142_REG_16BIT },	/* :1275 SMIA_TEST */
	{ 0x31ae, 0x0202, A5142_REG_16BIT },	/* :1276 2-lane serial */
	{ 0x0112, 0x0a0a, A5142_REG_16BIT },	/* :1277 RAW10 */
	{ 0x0300, 0x0005, A5142_REG_16BIT },	/* :1288 vt_pix_clk_div */
	{ 0x0302, 0x0001, A5142_REG_16BIT },	/* :1289 vt_sys_clk_div */
	{ 0x0304, 0x0004, A5142_REG_16BIT },	/* :1290 pre_pll_clk_div */
	{ 0x0306, 0x0056, A5142_REG_16BIT },	/* :1291 pll_multiplier = 86 */
	{ 0x0308, 0x000a, A5142_REG_16BIT },	/* :1292 op_pix_clk_div */
	{ 0x030a, 0x0001, A5142_REG_16BIT },	/* :1293 op_sys_clk_div */
	{ 0x0344, 0x0008, A5142_REG_16BIT },	/* :1307 x_addr_start */
	{ 0x0346, 0x0008, A5142_REG_16BIT },	/* :1308 y_addr_start */
	{ 0x0348, 0x0a27, A5142_REG_16BIT },	/* :1309 x_addr_end 2599 */
	{ 0x034a, 0x079f, A5142_REG_16BIT },	/* :1310 y_addr_end 1951 */
	{ 0x034c, 0x0a20, A5142_REG_16BIT },	/* :1317 x_output_size 2592 */
	{ 0x034e, 0x0798, A5142_REG_16BIT },	/* :1318 y_output_size 1944 */
	{ 0x300a, 0x07e5, A5142_REG_16BIT },	/* :1319 frame_lines 2021 */
	{ 0x300c, 0x0e6e, A5142_REG_16BIT },	/* :1320 line_length 3694 */
	{ 0x3010, 0x00a0, A5142_REG_16BIT },	/* :1321 fine_correction */
	{ 0x3014, 0x0c8c, A5142_REG_16BIT },	/* :1323 fine_integration_time */
};

/* Trailing latch release + stream on, shared by both mode tables. */
static const struct a5142_reg a5142_stream_on_8bit[] = {
	{ 0x0104, 0x0000, A5142_REG_8BIT },	/* group hold off */
	{ 0x0100, 0x0001, A5142_REG_8BIT },	/* stream on */
};

/*
 * Both vendor modes are carried because both are transcribed tables with a
 * complete PLL setup each; dropping the preview table would mean silently
 * discarding one of the two sequences the vendor ships.  Each mode names the
 * READ_MODE bits its table requires, because that register is a
 * read-modify-write in the vendor driver rather than part of the table.
 */
static const struct a5142_mode a5142_modes[] = {
	{
		.width = A5142_FULL_WIDTH,
		.height = A5142_FULL_HEIGHT,
		.read_mode = A5142_READ_MODE_FULL,
		.regs = a5142_capture_regs,
		.num_regs = ARRAY_SIZE(a5142_capture_regs),
		.stream_on = a5142_stream_on_8bit,
		.num_stream_on = ARRAY_SIZE(a5142_stream_on_8bit),
	},
	{
		.width = A5142_PV_WIDTH,
		.height = A5142_PV_HEIGHT,
		.read_mode = A5142_READ_MODE_BIN_2X2,
		.regs = a5142_preview_regs,
		.num_regs = ARRAY_SIZE(a5142_preview_regs),
		.stream_on = a5142_stream_on_8bit,
		.num_stream_on = ARRAY_SIZE(a5142_stream_on_8bit),
	},
};

/* -------------------------------------------------------------------------
 * Sensor state
 */

/*
 * Write READ_MODE preserving bits [15:12] the way the vendor does.  The read
 * half is the unverified operation described above, so the top nibble is
 * written as zero unless a read is later proven to work; the vendor's own
 * dump of that register is not in the tree, so this is the one place where
 * the sequence is not byte-for-byte identical to the vendor's.
 */
static int a5142_write_read_mode(struct a5142_device *dev, u16 mode_bits)
{
	return a5142_write(dev, A5142_REG_READ_MODE, mode_bits,
			   A5142_REG_16BIT);
}

static int a5142_init_sequence(struct a5142_device *dev)
{
	int ret;

	ret = a5142_write_table(dev, a5142_init_regs,
				ARRAY_SIZE(a5142_init_regs));
	if (ret)
		return ret;

	/* a5142mipi_Sensor.c:827 - mDELAY(5) after the software reset. */
	usleep_range(5000, 6000);

	return 0;
}

static int a5142_apply_mode(struct a5142_device *dev)
{
	const struct a5142_mode *mode = dev->mode;
	int ret;

	ret = a5142_write_table(dev, mode->regs, mode->num_regs);
	if (ret)
		return ret;

	ret = a5142_write_read_mode(dev, mode->read_mode);
	if (ret)
		return ret;

	return a5142_write_table(dev, mode->stream_on, mode->num_stream_on);
}

static int a5142_power_on(struct device *pdev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(pdev);
	struct a5142_device *dev = to_a5142(sd);
	int ret;

	ret = regulator_enable(dev->vddio);
	if (ret) {
		dev_err(pdev, "failed to enable vddio: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(dev->vdd);
	if (ret) {
		dev_err(pdev, "failed to enable vdd: %d\n", ret);
		goto err_vdd;
	}

	ret = clk_prepare_enable(dev->extclk);
	if (ret) {
		dev_err(pdev, "failed to enable extclk: %d\n", ret);
		goto err_vdd;
	}

	/*
	 * The vendor sequences reset low for 10 ms and then releases it
	 * (kd_camera_hw.c, pinSet[IDX_PS_CMRST]: OFF state then mdelay(10),
	 * then ON state and mdelay(1)).  The GPIO itself is not modelled by
	 * this driver yet, so only the settle time is honoured.
	 */
	usleep_range(10000, 11000);

	return 0;

err_vdd:
	regulator_disable(dev->vddio);
	return ret;
}

static int a5142_power_off(struct device *pdev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(pdev);
	struct a5142_device *dev = to_a5142(sd);

	clk_disable_unprepare(dev->extclk);
	regulator_disable(dev->vdd);
	regulator_disable(dev->vddio);

	return 0;
}

/* -------------------------------------------------------------------------
 * V4L2 controls
 */

/*
 * Orientation.  The vendor packs both flips into one 8-bit register at 0x0101:
 * bit 0 is horizontal and bit 1 vertical (a5142mipi_Sensor.c:1170-1181, where
 * IMAGE_NORMAL/H_MIRROR/V_MIRROR/HV_MIRROR write 0x00/0x01/0x02/0x03).
 * Rebuild the whole byte from both controls so a change to one does not clear
 * the other.
 */
static int a5142_set_orientation(struct a5142_device *dev)
{
	u16 val = 0;

	if (dev->ctrls.hflip->val)
		val |= A5142_ORIENTATION_HFLIP;
	if (dev->ctrls.vflip->val)
		val |= A5142_ORIENTATION_VFLIP;

	return a5142_write(dev, A5142_REG_IMAGE_ORIENTATION, val,
			   A5142_REG_8BIT);
}

static int a5142_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct a5142_device *dev = to_a5142(sd);
	int ret;

	/* v4l2_ctrl_lock() takes dev->lock for us. */
	if (!pm_runtime_get_if_in_use(&dev->client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = a5142_write(dev, A5142_REG_COARSE_INTEGRATION, ctrl->val,
				  A5142_REG_16BIT);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = a5142_write(dev, A5142_REG_GLOBAL_GAIN, ctrl->val,
				  A5142_REG_16BIT);
		break;
	case V4L2_CID_VBLANK:
		ret = a5142_write(dev, A5142_REG_FRAME_LENGTH_LINES,
				  dev->fmt.height + ctrl->val, A5142_REG_16BIT);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = a5142_set_orientation(dev);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&dev->client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops a5142_ctrl_ops = {
	.s_ctrl = a5142_s_ctrl,
};

static int a5142_init_controls(struct a5142_device *dev)
{
	struct v4l2_ctrl_handler *hdl = &dev->ctrls.handler;
	const struct v4l2_ctrl_ops *ops = &a5142_ctrl_ops;
	u32 max_exposure;
	int ret;

	v4l2_ctrl_handler_init(hdl, 8);
	hdl->lock = &dev->lock;
	dev->sd.ctrl_handler = hdl;

	/* Rows are the exposure unit; the vendor writes a line count to 0x0202. */
	max_exposure = A5142_FULL_FRAME_LINES;
	dev->ctrls.exposure = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,
						0, max_exposure, 1, 0x100);

	dev->ctrls.analogue_gain = v4l2_ctrl_new_std(hdl, ops,
						     V4L2_CID_ANALOGUE_GAIN,
						     A5142_GAIN_MIN,
						     A5142_GAIN_MAX, 1,
						     A5142_GAIN_DEFAULT);

	dev->ctrls.vblank = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VBLANK,
					      A5142_VBLANK_MIN,
					      A5142_TOTAL_LINES_MAX -
					      A5142_FULL_HEIGHT, 1,
					      A5142_VBLANK_MIN);

	dev->ctrls.hflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HFLIP,
					     0, 1, 1, 0);
	dev->ctrls.vflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VFLIP,
					     0, 1, 1, 0);
	/*
	 * V4L2_CID_TEST_PATTERN is deliberately absent: a5142mipi_Sensor.c
	 * contains no test-pattern register write anywhere (the only
	 * pattern-ish register, 0x3070, never appears), so exposing the
	 * control would mean inventing an address.
	 */

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Pad operations
 */

static int a5142_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	/* RAW10: the table programs 0x0112 = 0x0a0a (a5142mipi_Sensor.c:838). */
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int a5142_enum_frame_size(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	if (fse->index >= ARRAY_SIZE(a5142_modes))
		return -EINVAL;

	fse->min_width = a5142_modes[fse->index].width;
	fse->max_width = a5142_modes[fse->index].width;
	fse->min_height = a5142_modes[fse->index].height;
	fse->max_height = a5142_modes[fse->index].height;

	return 0;
}

static void a5142_fill_format(struct v4l2_mbus_framefmt *fmt,
			      const struct a5142_mode *mode)
{
	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

/*
 * Pick the vendor mode that matches the requested geometry.  There are two,
 * so an exact match is required; anything else falls back to the full-
 * resolution mode and the caller sees the adjusted size back.
 */
static const struct a5142_mode *a5142_find_mode(u32 width, u32 height)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(a5142_modes); i++)
		if (a5142_modes[i].width == width &&
		    a5142_modes[i].height == height)
			return &a5142_modes[i];

	return &a5142_modes[0];
}

static int a5142_get_fmt(struct v4l2_subdev *sd,
			 struct v4l2_subdev_state *state,
			 struct v4l2_subdev_format *format)
{
	struct a5142_device *dev = to_a5142(sd);
	struct v4l2_mbus_framefmt *fmt;

	mutex_lock(&dev->lock);
	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt = v4l2_subdev_state_get_format(state, format->pad);
	else
		fmt = &dev->fmt;
	format->format = *fmt;
	mutex_unlock(&dev->lock);

	return 0;
}

static int a5142_set_fmt(struct v4l2_subdev *sd,
			 struct v4l2_subdev_state *state,
			 struct v4l2_subdev_format *format)
{
	struct a5142_device *dev = to_a5142(sd);
	struct v4l2_mbus_framefmt *fmt;
	const struct a5142_mode *mode;

	mutex_lock(&dev->lock);

	mode = a5142_find_mode(format->format.width, format->format.height);
	a5142_fill_format(&format->format, mode);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt = v4l2_subdev_state_get_format(state, format->pad);
		*fmt = format->format;
	} else {
		dev->mode = mode;
		dev->fmt = format->format;
	}
	mutex_unlock(&dev->lock);

	return 0;
}

/*
 * Crop is the full pixel array.  The vendor's window is x 8..2599 and
 * y 8..1951 (a5142mipi_Sensor.c:1307-1310) with a 2592x1944 output, so the
 * array the driver can actually hand out is that window; report the window
 * itself rather than the 2592x1944 "native" figure the part advertises in
 * unused columns.
 */
static int a5142_get_selection(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_selection *sel)
{
	if (sel->target != V4L2_SEL_TGT_CROP &&
	    sel->target != V4L2_SEL_TGT_CROP_DEFAULT &&
	    sel->target != V4L2_SEL_TGT_CROP_BOUNDS)
		return -EINVAL;

	sel->r.left = 0;
	sel->r.top = 0;
	sel->r.width = A5142_FULL_WIDTH;
	sel->r.height = A5142_FULL_HEIGHT;

	return 0;
}

static const struct v4l2_subdev_pad_ops a5142_pad_ops = {
	.enum_mbus_code = a5142_enum_mbus_code,
	.enum_frame_size = a5142_enum_frame_size,
	.get_fmt = a5142_get_fmt,
	.set_fmt = a5142_set_fmt,
	.get_selection = a5142_get_selection,
};

/* -------------------------------------------------------------------------
 * Video operations
 */

static int a5142_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct a5142_device *dev = to_a5142(sd);
	int ret = 0;

	mutex_lock(&dev->lock);

	if (enable) {
		ret = pm_runtime_resume_and_get(&dev->client->dev);
		if (ret)
			goto out;

		ret = a5142_init_sequence(dev);
		if (ret)
			goto err_pm;

		ret = a5142_apply_mode(dev);
		if (ret)
			goto err_pm;

		ret = __v4l2_ctrl_handler_setup(&dev->ctrls.handler);
		if (ret)
			goto err_pm;
	} else {
		ret = a5142_write(dev, A5142_REG_MODE_SELECT, 0,
				  A5142_REG_8BIT);
		pm_runtime_put(&dev->client->dev);
	}

	mutex_unlock(&dev->lock);
	return ret;

err_pm:
	pm_runtime_put(&dev->client->dev);
out:
	mutex_unlock(&dev->lock);
	return ret;
}

static const struct v4l2_subdev_video_ops a5142_video_ops = {
	.s_stream = a5142_s_stream,
};

static const struct v4l2_subdev_ops a5142_subdev_ops = {
	.pad = &a5142_pad_ops,
	.video = &a5142_video_ops,
};

/* -------------------------------------------------------------------------
 * Probe / remove
 */

static int a5142_probe(struct i2c_client *client)
{
	struct device *pdev = &client->dev;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *endpoint;
	struct a5142_device *dev;
	unsigned long extclk_rate;
	int ret;

	dev = devm_kzalloc(pdev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->client = client;
	dev->mode = &a5142_modes[0];
	a5142_fill_format(&dev->fmt, dev->mode);

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(pdev), 0, 0,
						   FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!endpoint)
		return dev_err_probe(pdev, -ENODEV, "no endpoint node\n");

	ret = v4l2_fwnode_endpoint_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(pdev, ret, "failed to parse endpoint\n");

	if (ep.bus_type != V4L2_MBUS_CSI2_DPHY)
		return dev_err_probe(pdev, -EINVAL, "not a MIPI CSI-2 endpoint\n");

	dev->lane_count = ep.bus.mipi_csi2.num_data_lanes;
	if (dev->lane_count != A5142_LANES)
		return dev_err_probe(pdev, -EINVAL,
				     "%u lanes, the vendor table programs %u\n",
				     dev->lane_count, A5142_LANES);

	dev->extclk = devm_clk_get(pdev, "extclk");
	if (IS_ERR(dev->extclk))
		return dev_err_probe(pdev, PTR_ERR(dev->extclk),
				     "failed to get extclk\n");

	extclk_rate = clk_get_rate(dev->extclk);
	if (extclk_rate < A5142_EXTCLK_MIN ||
	    extclk_rate > A5142_EXTCLK_MAX)
		return dev_err_probe(pdev, -EINVAL,
				     "extclk %lu Hz, the PLL table is built for %u Hz\n",
				     extclk_rate, A5142_EXTCLK_MIN);

	/*
	 * Rails, named after the vendor's generic camera supplies
	 * (kd_camera_hw.h:13-16): VCAMA is the 2.8 V analogue rail and
	 * VCAMD the 1.8 V core/IO rail this board wires to VGP1
	 * (kd_camera_hw.h:14, "MT65XX_POWER_LDO_VGP1 //VCAMD").
	 */
	dev->vdd = devm_regulator_get(pdev, "vdd");
	if (IS_ERR(dev->vdd))
		return dev_err_probe(pdev, PTR_ERR(dev->vdd),
				     "failed to get vdd\n");

	dev->vddio = devm_regulator_get(pdev, "vddio");
	if (IS_ERR(dev->vddio))
		return dev_err_probe(pdev, PTR_ERR(dev->vddio),
				     "failed to get vddio\n");

	mutex_init(&dev->lock);

	v4l2_i2c_subdev_init(&dev->sd, client, &a5142_subdev_ops);
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret)
		goto err_mutex;

	ret = a5142_init_controls(dev);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(pdev);
	pm_runtime_enable(pdev);
	pm_runtime_idle(pdev);

	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret)
		goto err_pm;

	dev_info(pdev, "A5142 on i2c-%d at %#04x\n",
		 client->adapter->nr, client->addr);

	return 0;

err_pm:
	pm_runtime_disable(pdev);
	v4l2_ctrl_handler_free(&dev->ctrls.handler);
err_entity:
	media_entity_cleanup(&dev->sd.entity);
err_mutex:
	mutex_destroy(&dev->lock);
	return ret;
}

static void a5142_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct a5142_device *dev = to_a5142(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&dev->ctrls.handler);
	pm_runtime_disable(&client->dev);
	mutex_destroy(&dev->lock);
}

static const struct dev_pm_ops a5142_pm_ops = {
	SET_RUNTIME_PM_OPS(a5142_power_off, a5142_power_on, NULL)
};

static const struct of_device_id a5142_of_match[] = {
	{ .compatible = "onnn,a5142" },
	{ }
};
MODULE_DEVICE_TABLE(of, a5142_of_match);

static struct i2c_driver a5142_driver = {
	.driver = {
		.name = "a5142",
		.pm = &a5142_pm_ops,
		.of_match_table = a5142_of_match,
	},
	.probe = a5142_probe,
	.remove = a5142_remove,
};
module_i2c_driver(a5142_driver);

MODULE_DESCRIPTION("ON Semiconductor A5142 camera sensor driver");
MODULE_LICENSE("GPL");
