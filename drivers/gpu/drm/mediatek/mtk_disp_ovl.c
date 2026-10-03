// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"

#define DISP_REG_OVL_STA				0x0000
#define DISP_REG_OVL_INTEN					0x0004
/*
 * OVL_INTEN (0x14003004) and OVL_INTSTA (0x14003008) share one bit map.
 * The data sheet's "Bit 15..0" row for both registers names exactly
 * these twelve mnemonics and leaves 12..31 unnamed, so nothing above
 * BIT(11) may ever appear in a mask derived from it:
 *
 *   bit  0  OVL_REG_CMT_INT		shadow -> working register commit done
 *   bit  1  OVL_FME_CPL_INT		frame complete
 *   bit  2  OVL_FME_UND_INT		frame underflow
 *   bit  3  OVL_FME_SWRST_DONE_INT	SW reset done
 *   bit  4  OVL_RDMA0_EOF_ABNORMAL_INT
 *   bit  5  OVL_RDMA1_EOF_ABNORMAL_INT
 *   bit  6  OVL_RDMA2_EOF_ABNORMAL_INT
 *   bit  7  OVL_RDMA3_EOF_ABNORMAL_INT
 *   bit  8  OVL_RDMA0_FIFO_UND_INT
 *   bit  9  OVL_RDMA1_FIFO_UND_INT
 *   bit 10  OVL_RDMA2_FIFO_UND_INT
 *   bit 11  OVL_RDMA3_FIFO_UND_INT
 *
 * Every status bit is write-0-to-clear ("Cleared by writing 0 to it;
 * writing 1 is useless"), and INTEN mirrors INTSTA one-for-one, so a
 * driver that enables a bit is obliged to acknowledge it again in the
 * ISR.  OVL_INT_ALL below is the union of all twelve.
*/
#define OVL_REG_CMT_INT						BIT(0)
#define OVL_FME_CPL_INT							BIT(1)
#define OVL_FME_UND_INT							BIT(2)
#define OVL_FME_SWRST_DONE_INT					BIT(3)
#define OVL_RDMA0_EOF_ABNORMAL_INT	BIT(4)
#define OVL_RDMA1_EOF_ABNORMAL_INT	BIT(5)
#define OVL_RDMA2_EOF_ABNORMAL_INT	BIT(6)
#define OVL_RDMA3_EOF_ABNORMAL_INT	BIT(7)
#define OVL_RDMA0_FIFO_UND_INT			BIT(8)
#define OVL_RDMA1_FIFO_UND_INT			BIT(9)
#define OVL_RDMA2_FIFO_UND_INT			BIT(10)
#define OVL_RDMA3_FIFO_UND_INT			BIT(11)

/*
 * Every OVL_INTSTA bit this block can raise.  The ISR acknowledges
 * reg & int_all_mask so that whatever mtk_ovl_enable_vblank() switched
 * on in OVL_INTEN is also deasserted again - see
 * mtk_disp_ovl_irq_handler().
*/
#define OVL_INT_ALL \
(OVL_REG_CMT_INT | OVL_FME_CPL_INT | OVL_FME_UND_INT | \
OVL_FME_SWRST_DONE_INT | \
OVL_RDMA0_EOF_ABNORMAL_INT | OVL_RDMA1_EOF_ABNORMAL_INT | \
OVL_RDMA2_EOF_ABNORMAL_INT | OVL_RDMA3_EOF_ABNORMAL_INT | \
OVL_RDMA0_FIFO_UND_INT | OVL_RDMA1_FIFO_UND_INT | \
OVL_RDMA2_FIFO_UND_INT | OVL_RDMA3_FIFO_UND_INT)

#define DISP_REG_OVL_INTSTA			0x0008

#define DISP_REG_OVL_EN				0x000c
#define DISP_REG_OVL_RST			0x0014

#define DISP_REG_OVL_ROI_SIZE			0x0020
#define DISP_REG_OVL_DATAPATH_CON		0x0024
#define OVL_LAYER_SMI_ID_EN				BIT(0)
#define OVL_BGCLR_SEL_IN				BIT(2)
#define OVL_LAYER_AFBC_EN(n)				BIT(4+n)
#define DISP_REG_OVL_ROI_BGCLR			0x0028
#define DISP_REG_OVL_SRC_CON			0x002c
#define DISP_REG_OVL_CON(n)			(0x0030 + 0x20 * (n))
#define DISP_REG_OVL_SRC_SIZE(n)		(0x0038 + 0x20 * (n))
#define DISP_REG_OVL_OFFSET(n)			(0x003c + 0x20 * (n))
#define DISP_REG_OVL_PITCH_MSB(n)		(0x0040 + 0x20 * (n))
#define OVL_PITCH_MSB_2ND_SUBBUF			BIT(16)
#define DISP_REG_OVL_PITCH(n)			(0x0044 + 0x20 * (n))
#define OVL_CONST_BLEND					BIT(28)
#define DISP_REG_OVL_RDMA_CTRL(n)		(0x00c0 + 0x20 * (n))
#define DISP_REG_OVL_RDMA_GMC(n)		(0x00c8 + 0x20 * (n))
#define DISP_REG_OVL_CLRFMT_EXT			0x02d0
#define OVL_CON_CLRFMT_BIT_DEPTH_MASK(n)		(GENMASK(1, 0) << (4 * (n)))
#define OVL_CON_CLRFMT_BIT_DEPTH(depth, n)		((depth) << (4 * (n)))
#define OVL_CON_CLRFMT_8_BIT				(0)
#define OVL_CON_CLRFMT_10_BIT				(1)
/*
 * data->addr is the byte offset of layer 0's ADDR register within the OVL
 * block, not the block base.  The stock header has DISP_REG_OVL_L0_ADDR at
 * DISPSYS_OVL_BASE + 0x0040 and the other layers at +0x0060, +0x0080 and
 * +0x00a0, i.e. 0x20 apart - which is why DISP_REG_OVL_ADDR() below adds
 * 0x20 * n to this.  MT2701 and MT6589 share the layout, so both use the
 * same 0x40; MT8173's OVL sits later in the block and uses 0x0f40.
 */
#define DISP_REG_OVL_ADDR_MT2701		0x0040
#define DISP_REG_OVL_ADDR_MT8173		0x0f40
#define DISP_REG_OVL_ADDR(ovl, n)		((ovl)->data->addr + 0x20 * (n))
#define DISP_REG_OVL_HDR_ADDR(ovl, n)		((ovl)->data->addr + 0x20 * (n) + 0x04)
#define DISP_REG_OVL_HDR_PITCH(ovl, n)		((ovl)->data->addr + 0x20 * (n) + 0x08)

#define GMC_THRESHOLD_BITS	16
#define GMC_THRESHOLD_HIGH	((1 << GMC_THRESHOLD_BITS) / 4)
#define GMC_THRESHOLD_LOW	((1 << GMC_THRESHOLD_BITS) / 8)

#define OVL_CON_CLRFMT_MAN	BIT(23)
#define OVL_CON_BYTE_SWAP	BIT(24)

/* OVL_CON_RGB_SWAP works only if OVL_CON_CLRFMT_MAN is enabled */
#define OVL_CON_RGB_SWAP	BIT(25)

#define OVL_CON_CLRFMT_RGB	(1 << 12)
#define OVL_CON_CLRFMT_ARGB8888	(2 << 12)
#define OVL_CON_CLRFMT_RGBA8888	(3 << 12)
#define OVL_CON_CLRFMT_ABGR8888	(OVL_CON_CLRFMT_ARGB8888 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_BGRA8888	(OVL_CON_CLRFMT_RGBA8888 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_UYVY	(4 << 12)
#define OVL_CON_CLRFMT_YUYV	(5 << 12)
#define OVL_CON_CLRFMT_PARGB8888 ((3 << 12) | OVL_CON_CLRFMT_MAN)
#define OVL_CON_CLRFMT_PABGR8888 (OVL_CON_CLRFMT_PARGB8888 | OVL_CON_RGB_SWAP)
#define OVL_CON_CLRFMT_PBGRA8888 (OVL_CON_CLRFMT_PARGB8888 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_PRGBA8888 (OVL_CON_CLRFMT_PABGR8888 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_RGB565(ovl)	((ovl)->data->fmt_rgb565_is_0 ? \
					0 : OVL_CON_CLRFMT_RGB)
#define OVL_CON_CLRFMT_RGB888(ovl)	((ovl)->data->fmt_rgb565_is_0 ? \
					OVL_CON_CLRFMT_RGB : 0)
#define	OVL_CON_AEN		BIT(8)
#define	OVL_CON_ALPHA		0xff
#define	OVL_CON_VIRT_FLIP	BIT(9)
#define	OVL_CON_HORZ_FLIP	BIT(10)

#define OVL_COLOR_ALPHA		GENMASK(31, 24)

/* MT6589 specific registers for YUV-to-RGB conversion matrix */
#define DISP_REG_OVL_Y2R_BASE(n)	(0x0134 + 0x28 * (n))
#define Y2R_R0				0x00
#define Y2R_R1				0x04
#define Y2R_G0				0x08
#define Y2R_G1				0x0C
#define Y2R_B0				0x10
#define Y2R_B1				0x14
#define Y2R_YUV_A0			0x18
#define Y2R_YUV_A1			0x1C
#define Y2R_RGB_A0			0x20
#define Y2R_RGB_A1			0x24

/*
 * BT.601 YUV2RGB conversion matrix for the OVL Y2R engine.
 *
 * The hardware takes a 5x3 coefficient table in the same layout as the
 * downstream kernel's ddp_matrix_para.h: rows R/G/B hold {MY, MU, MV}
 * multipliers (13-bit sign+2.10 fixed point), row 3 the {YA, UA, VA}
 * offsets and row 4 the {RA, GA, BA} output gains.  This is
 * yuv2rgb_601_0_0 (full-range input, full-range output).
 */
static const s16 mt6589_yuv2rgb_coef[5][3] = {
	{ 0x0400, 0x0000, 0x059b },	/*     1,      0, 1.402 */
	{ 0x0400, 0x1ea0, 0x1d25 },	/*     1, -0.3341, -0.7141 */
	{ 0x0400, 0x0716, 0x0000 },	/*     1,   1.772,      0 */
	{ 0x0000, 0x0180, 0x0180 },	/*     0,   -128,   -128 */
	{ 0x0000, 0x0000, 0x0000 },	/* gains are written separately */
};

static inline bool is_10bit_rgb(u32 fmt)
{
	switch (fmt) {
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRA1010102:
		return true;
	}
	return false;
}

static const u32 mt8173_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
};

static const u32 mt8195_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_ARGB2101010,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_BGRX1010102,
	DRM_FORMAT_BGRA1010102,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_RGBX1010102,
	DRM_FORMAT_RGBA1010102,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
};

struct mtk_disp_ovl_data {
	unsigned int addr;
	unsigned int gmc_bits;
	unsigned int layer_nr;
	bool fmt_rgb565_is_0;
	bool smi_id_en;
	bool supports_afbc;
	bool has_const_blend;
	bool set_layer_src;
	unsigned int vblank_en_mask;
	/*
	 * int_all_mask is the whole set of OVL_INTSTA bits this block can
	 * raise.  It is ANDed with the latched status to decide both what
	 * to acknowledge and whether the line is ours at all, so it must
	 * cover every bit vblank_en_mask is able to enable.  fme_cpl_bit
	 * is the frame complete status bit - the only one that means a
	 * frame was actually produced, and therefore the only one
	 * allowed to fire vblank_cb.
	 *
	 * Both are required and every SoC below states both explicitly.
	 * A zero is a real, deliberate value, not "unset" - it says this
	 * block raises no status bit that this driver can acknowledge,
	 * which is true of the eight SoCs that never write a non-zero
	 * value to OVL_INTEN (see mtk_disp_ovl_check_data()).
	 */
	unsigned int int_all_mask;
	unsigned int fme_cpl_bit;
	unsigned int fme_und_bit;
	unsigned int rdma0_eof_abn_bit;
	unsigned int rdma1_eof_abn_bit;
	unsigned int rdma0_fifo_und_bit;
	unsigned int rdma1_fifo_und_bit;
	unsigned int (*fmt_convert)(unsigned int fmt, unsigned int blend_mode);
	const u32 blend_modes;
	const u32 *formats;
	size_t num_formats;
	bool supports_clrfmt_ext;
};

/*
 * The vblank trigger for a SoC whose OVL_INTSTA bit map this driver does
 * not describe.  Such a SoC cannot say which bit means "a frame was
 * produced", so every status bit the block latches counts as a frame
 * boundary - the behaviour this driver has always had for them, and the
 * only choice that cannot silently stop a display that is working today.
 *
 * This is spelled out per SoC rather than defaulted at run time so that
 * the value in the data is the value the ISR uses.
 */
#define OVL_FME_CPL_ANY		GENMASK(31, 0)

/*
 * struct mtk_disp_ovl - DISP_OVL driver structure
 * @crtc: associated crtc to report vblank events to
 * @data: platform data
 */
struct mtk_disp_ovl {
	struct drm_crtc			*crtc;
	struct clk_bulk_data		*clks;
	struct reset_control		*rstc;
	int				num_clks;
	void __iomem			*regs;
	struct cmdq_client_reg		cmdq_reg;
	const struct mtk_disp_ovl_data	*data;
	void				(*vblank_cb)(void *data);
	void				*vblank_cb_data;
};

static irqreturn_t mtk_disp_ovl_irq_handler(int irq, void *dev_id)
{
	struct mtk_disp_ovl *priv = dev_id;
	u32 reg = readl(priv->regs + DISP_REG_OVL_INTSTA);
	u32 handled;

	/*
	 * A SoC that owns at least one status bit (int_all_mask non-zero)
	 * must show one of them asserted for the line to be ours.  Only
	 * then can we return IRQ_NONE without having acknowledged anything,
	 * because there is nothing of ours in reg to clear.
	 *
	 * int_all_mask == 0 is not a sentinel for "map unknown" - it is the
	 * explicit statement that this block raises nothing this driver can
	 * acknowledge, and so there is nothing to reject it on.  Testing the
	 * mask alone, as "if (!(reg & int_all_mask))", would read that as
	 * "no bit matches" and eject every such SoC from the handler on
	 * every invocation - the regression that came with int_all_mask in
	 * the first place, since it stopped those SoCs clearing their
	 * status and stopped their vblank callback from ever running.  Hence
	 * the mask has to be known non-zero before the intersection is
	 * allowed to veto.
	 *
	 * The invariant that lets this be written this way is checked at
	 * probe: int_all_mask covers every bit vblank_en_mask can enable,
	 * so a zero mask implies a zero enable mask, so those SoCs never
	 * raise an OVL interrupt through this driver at all.
	 */
	if (priv->data->int_all_mask && !(reg & priv->data->int_all_mask))
		return IRQ_NONE;

	/*
 * Report OVL_STA alongside the interrupt status.  Bit 0 is OVL_RUN
 * and bit 1 is RDMA0_IDLE, so one line separates the possibilities:
 * run=0 with RDMA0 idle means the engine never started, run=1 with
 * RDMA0 not idle means RDMA stalled fetching, both idle means nothing
 * is triggering start-of-frame.
	 */
	if (reg & (priv->data->fme_und_bit | priv->data->rdma0_eof_abn_bit)) {
		u32 sta = readl(priv->regs + DISP_REG_OVL_STA);

		pr_err("OVL: underflow intsta=%#x sta=%#x (run=%d rdma0_idle=%d)\n",
		       reg, sta, !!(sta & 1), !!(sta & 0x2));
	}

	if (reg & priv->data->fme_und_bit)
		pr_err("OVL: OVL frame underflow\n");
	if (reg & priv->data->rdma0_eof_abn_bit)
		pr_err("OVL: RDMA0 didn't complete frame\n");
	if (reg & priv->data->rdma1_eof_abn_bit)
		pr_err("OVL: RDMA1 didn't complete frame\n");
	if (reg & priv->data->rdma0_fifo_und_bit)
		pr_err("OVL: RDMA0 FIFO underflow\n");
	if (reg & priv->data->rdma1_fifo_und_bit)
		pr_err("OVL: RDMA1 FIFO underflow\n");

	/*
 * Acknowledge every status bit this block can raise, not merely the
 * faults printed above.
 *
 * OVL_INTSTA is write-0-to-clear: the data sheet says "cleared by
 * writing 0 to it; writing 1 is useless".  OVL_INTEN mirrors that
 * bit map one-for-one, and mtk_ovl_enable_vblank() programs it
 * with vblank_en_mask = 0xf, i.e. REG_CMT (bit 0), FME_CPL (bit
 * 1), FME_UND (bit 2) and FME_SWRST_DONE (bit 3).  The old clear
 * list held only bits 2, 4, 5, 8 and 9, so frame complete stayed
 * latched from the first frame onwards: the line is level
 * triggered (IRQF_TRIGGER_NONE, as wired up in DT) and the handler
 * re-entered forever, printing the identical intsta every frame.
 *
 * Masking with int_all_mask rather than with an ad-hoc list of
 * fault bits is what makes this hold structurally: "we enabled it,
 * so we must acknowledge it" survives any future change to
 * vblank_en_mask or to the individual fault-bit fields, because
 * the acknowledged set is a superset of whatever the enable mask
 * can select.
 *
 * The write is inverted, so every bit we are not acknowledging
 * goes out as 1 and a condition arriving after the readl above is
 * left alone rather than wiped.
	 */
	handled = reg & priv->data->int_all_mask;
	writel(~handled, priv->regs + DISP_REG_OVL_INTSTA);

	/*
	 * On a SoC whose interrupt map is known (MT6589) fme_cpl_bit names
	 * one bit and only a completed frame is a vblank: underflow and the
	 * RDMA faults are errors, already reported above, and firing the
	 * vblank callback for them would hand the CRTC a frame that never
	 * arrived.  The eight SoCs whose map this driver does not describe
	 * state OVL_FME_CPL_ANY, which counts every status bit as a frame
	 * boundary - the behaviour they have always had.  Either way a
	 * frame complete with no callback registered is still acknowledged
	 * by the clear above and cannot re-trigger.
	 */
	if (priv->vblank_cb && (reg & priv->data->fme_cpl_bit))
		priv->vblank_cb(priv->vblank_cb_data);

	/*
 * The line is ours and we have just deasserted its status bits, so
 * returning IRQ_NONE here would wrongly hand an already
 * acknowledged interrupt to another handler.  IRQ_HANDLED even
 * with no callback registered - the clear is the handling.
	 */
	return IRQ_HANDLED;
}

void mtk_ovl_register_vblank_cb(struct device *dev,
				void (*vblank_cb)(void *),
				void *vblank_cb_data)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	ovl->vblank_cb = vblank_cb;
	ovl->vblank_cb_data = vblank_cb_data;
}

void mtk_ovl_unregister_vblank_cb(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	ovl->vblank_cb = NULL;
	ovl->vblank_cb_data = NULL;
}

void mtk_ovl_enable_vblank(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	writel(0x0, ovl->regs + DISP_REG_OVL_INTSTA);
	writel_relaxed(ovl->data->vblank_en_mask,
		       ovl->regs + DISP_REG_OVL_INTEN);
}

void mtk_ovl_disable_vblank(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	writel_relaxed(0x0, ovl->regs + DISP_REG_OVL_INTEN);
}

u32 mtk_ovl_get_blend_modes(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->blend_modes;
}

const u32 *mtk_ovl_get_formats(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->formats;
}

size_t mtk_ovl_get_num_formats(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->num_formats;
}

bool mtk_ovl_is_afbc_supported(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->supports_afbc;
}

int mtk_ovl_clk_enable(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(ovl->num_clks, ovl->clks);
}

void mtk_ovl_clk_disable(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(ovl->num_clks, ovl->clks);
}

void mtk_ovl_start(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	if (ovl->data->smi_id_en) {
		unsigned int reg;

		reg = readl(ovl->regs + DISP_REG_OVL_DATAPATH_CON);
		reg = reg | OVL_LAYER_SMI_ID_EN;
		writel_relaxed(reg, ovl->regs + DISP_REG_OVL_DATAPATH_CON);
	}

	/*
	 * Enable the interrupts before the engine, as OVLStart() does:
	 * DISP_REG_SET_FIELD(OVL_INTEN, 0x0f) then OVL_EN = 1.
	 *
	 * 0x0f covers the four low interrupt-enable bits - REG_CMT_INTEN,
	 * FME_CPL_INTEN, FME_UND_INTEN and OVL_SWRS_INTEN - and that map is
	 * documented for MT6589 only, which is where the value lives.  It is
	 * read from ->data here rather than written as a literal so that this
	 * shared path cannot enable MT6589's interrupt bits on an SoC whose
	 * map is different or unknown.  Nothing in this driver ever set
	 * OVL_INTEN outside the reset and stop paths, so on MT6589 the engine
	 * used to run with every interrupt masked and the underflow condition
	 * was reported without the completion that should follow it.
	 *
	 * A SoC that does not set vblank_en_mask keeps the interrupts
	 * masked here, which is exactly what it did before this write
	 * existed - mtk_ovl_enable_vblank() is what turns them on for it.
	 */
	writel_relaxed(ovl->data->vblank_en_mask,
		       ovl->regs + DISP_REG_OVL_INTEN);
	writel_relaxed(0x1, ovl->regs + DISP_REG_OVL_EN);
}

void mtk_ovl_stop(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	/*
	 * Mask the interrupts before stopping the engine, as OVLStop() does
	 * (INTEN = 0, EN = 0).  Stopping the engine with a latched status
	 * still enabled makes the level-triggered interrupt fire again
	 * immediately and keeps re-entering the handler.
	 */
	writel_relaxed(0x0, ovl->regs + DISP_REG_OVL_INTEN);
	writel_relaxed(0x0, ovl->regs + DISP_REG_OVL_EN);
	if (ovl->data->smi_id_en) {
		unsigned int reg;

		reg = readl(ovl->regs + DISP_REG_OVL_DATAPATH_CON);
		reg = reg & ~OVL_LAYER_SMI_ID_EN;
		writel_relaxed(reg, ovl->regs + DISP_REG_OVL_DATAPATH_CON);
	}
}

static void mtk_ovl_set_afbc(struct mtk_disp_ovl *ovl, struct cmdq_pkt *cmdq_pkt,
			     int idx, bool enabled)
{
	mtk_ddp_write_mask(cmdq_pkt, enabled ? OVL_LAYER_AFBC_EN(idx) : 0,
			   &ovl->cmdq_reg, ovl->regs,
			   DISP_REG_OVL_DATAPATH_CON, OVL_LAYER_AFBC_EN(idx));
}

static void mtk_ovl_set_bit_depth(struct device *dev, int idx, u32 format,
				  struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int bit_depth = OVL_CON_CLRFMT_8_BIT;

	if (!ovl->data->supports_clrfmt_ext)
		return;

	if (is_10bit_rgb(format))
		bit_depth = OVL_CON_CLRFMT_10_BIT;

	mtk_ddp_write_mask(cmdq_pkt, OVL_CON_CLRFMT_BIT_DEPTH(bit_depth, idx),
			   &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_CLRFMT_EXT,
			   OVL_CON_CLRFMT_BIT_DEPTH_MASK(idx));
}

static void mt6589_ovl_write_yuv_matrix(struct mtk_disp_ovl *ovl,
					unsigned int idx, struct cmdq_pkt *cmdq_pkt)
{
	void __iomem *base = ovl->regs;
	unsigned int reg_base = DISP_REG_OVL_Y2R_BASE(idx);

	mtk_ddp_write(cmdq_pkt,
		      (mt6589_yuv2rgb_coef[0][1] << 16) | (mt6589_yuv2rgb_coef[0][0] & 0x1FFF),
		      &ovl->cmdq_reg, base, reg_base + Y2R_R0);
	mtk_ddp_write(cmdq_pkt, mt6589_yuv2rgb_coef[0][2] & 0x1FFF,
		      &ovl->cmdq_reg, base, reg_base + Y2R_R1);
	mtk_ddp_write(cmdq_pkt,
		      (mt6589_yuv2rgb_coef[1][1] << 16) | (mt6589_yuv2rgb_coef[1][0] & 0x1FFF),
		      &ovl->cmdq_reg, base, reg_base + Y2R_G0);
	mtk_ddp_write(cmdq_pkt, mt6589_yuv2rgb_coef[1][2] & 0x1FFF,
		      &ovl->cmdq_reg, base, reg_base + Y2R_G1);
	mtk_ddp_write(cmdq_pkt,
		      (mt6589_yuv2rgb_coef[2][1] << 16) | (mt6589_yuv2rgb_coef[2][0] & 0x1FFF),
		      &ovl->cmdq_reg, base, reg_base + Y2R_B0);
	mtk_ddp_write(cmdq_pkt, mt6589_yuv2rgb_coef[2][2] & 0x1FFF,
		      &ovl->cmdq_reg, base, reg_base + Y2R_B1);
	/*
 * Offset fields are 9-bit signed (sign + 8.0): the U/V offsets of
 * -128 must be programmed as 0x180 like the downstream kernel does.
	 */
	mtk_ddp_write(cmdq_pkt,
		      (mt6589_yuv2rgb_coef[3][1] << 16) | mt6589_yuv2rgb_coef[3][0],
		      &ovl->cmdq_reg, base, reg_base + Y2R_YUV_A0);
	mtk_ddp_write(cmdq_pkt, mt6589_yuv2rgb_coef[3][2],
			      &ovl->cmdq_reg, base, reg_base + Y2R_YUV_A1);
	mtk_ddp_write(cmdq_pkt, mt6589_yuv2rgb_coef[4][0],
		      &ovl->cmdq_reg, base, reg_base + Y2R_RGB_A0);
	mtk_ddp_write(cmdq_pkt, 0, &ovl->cmdq_reg, base, reg_base + Y2R_RGB_A1);
}

void mtk_ovl_config(struct device *dev, unsigned int w,
		    unsigned int h, unsigned int vrefresh,
		    unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	/*
 * Soft reset first, wait for it to take effect, then release it, and
 * only then program the mode. The order matters: the reset clears the
 * engine's configuration, so writing OVL_ROI_SIZE and OVL_ROI_BGCLR
 * before asserting it left the block with no frame size at all, and
 * the stock driver has the same order - OVLReset() runs and only then
 * OVLConfig() writes OVL_ROI_SIZE (ddp_ovl.c).
 *
 * The data sheet requires polling the engine's run bit until it reads
 * 0 before the reset is released.
	 */
	mtk_ddp_write(cmdq_pkt, 0x1, &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_RST);

	if (!cmdq_pkt) {
		unsigned int i;

		for (i = 0; i < 10000; i++) {
			/*
 * Wait for OVL_STA bit0 (OVL_RUN) to drop. This used
 * to poll OVL_INTSTA bit0, but that is OVL_REG_CMT -
 * a latched event, not the run state - so the loop
 * could exit while the engine was still running and
 * the reset had not taken effect.
			 */
			if (!(readl(ovl->regs + DISP_REG_OVL_STA) & BIT(0)))
				break;
			cpu_relax();
		}
	}

	mtk_ddp_write(cmdq_pkt, 0x0, &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_RST);

	if (w != 0 && h != 0)
		mtk_ddp_write_relaxed(cmdq_pkt, h << 16 | w, &ovl->cmdq_reg, ovl->regs,
				      DISP_REG_OVL_ROI_SIZE);

	/*
 * The background color must be opaque black (ARGB),
 * otherwise the alpha blending will have no effect
	 */
	mtk_ddp_write_relaxed(cmdq_pkt, OVL_COLOR_ALPHA, &ovl->cmdq_reg,
			      ovl->regs, DISP_REG_OVL_ROI_BGCLR);
}

unsigned int mtk_ovl_layer_nr(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->layer_nr;
}

unsigned int mtk_ovl_supported_rotations(struct device *dev)
{
	return DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180 |
	       DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;
}

int mtk_ovl_layer_check(struct device *dev, unsigned int idx,
			struct mtk_plane_state *mtk_state)
{
	struct drm_plane_state *state = &mtk_state->base;

	/* check if any unsupported rotation is set */
	if (state->rotation & ~mtk_ovl_supported_rotations(dev))
		return -EINVAL;

	/*
 * TODO: Rotating/reflecting YUV buffers is not supported at this time.
 *	 Only RGB[AX] variants are supported.
 *	 Since DRM_MODE_ROTATE_0 means "no rotation", we should not
 *	 reject layers with this property.
	 */
	if (state->fb->format->is_yuv && (state->rotation & ~DRM_MODE_ROTATE_0))
		return -EINVAL;

	return 0;
}

void mtk_ovl_layer_on(struct device *dev, unsigned int idx,
		      struct cmdq_pkt *cmdq_pkt)
{
	unsigned int gmc_thrshd_l;
	unsigned int gmc_thrshd_h;
	unsigned int gmc_value;
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	mtk_ddp_write(cmdq_pkt, 0x1, &ovl->cmdq_reg, ovl->regs,
		      DISP_REG_OVL_RDMA_CTRL(idx));
	gmc_thrshd_l = GMC_THRESHOLD_LOW >>
		      (GMC_THRESHOLD_BITS - ovl->data->gmc_bits);
	gmc_thrshd_h = GMC_THRESHOLD_HIGH >>
		      (GMC_THRESHOLD_BITS - ovl->data->gmc_bits);
	/*
 * OVL_RDMA0_MEM_GMC_SETTING holds RDMA0_EN_THRD in bits [9:0] and
 * RDMA0_DISEN_THRD in bits [25:16] - two 10-bit fields in units of
 * 16 bytes, and the data sheet requires EN_THRD to be *smaller*
 * than DISEN_THRD.
 *
 * Both branches above put the high threshold in both fields, so the
 * two were equal and the requirement was violated; and the 8-bit
 * branch packed four 8-bit values into 32 bits, which is a different
 * layout from this one. Set the two fields from the two thresholds,
 * masked to the field width.
	 */
	gmc_value = (gmc_thrshd_l & GENMASK(9, 0)) |
		    ((gmc_thrshd_h & GENMASK(9, 0)) << 16);
	mtk_ddp_write(cmdq_pkt, gmc_value,
		      &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_RDMA_GMC(idx));

	/* For MT6589, explicitly set layer source to memory (0) */
	if (ovl->data->set_layer_src)
		mtk_ddp_write_mask(cmdq_pkt, 0, &ovl->cmdq_reg, ovl->regs,
				   DISP_REG_OVL_CON(idx), GENMASK(29, 28));

	mtk_ddp_write_mask(cmdq_pkt, BIT(idx), &ovl->cmdq_reg, ovl->regs,
			   DISP_REG_OVL_SRC_CON, BIT(idx));
}

void mtk_ovl_layer_off(struct device *dev, unsigned int idx,
		       struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	mtk_ddp_write_mask(cmdq_pkt, 0, &ovl->cmdq_reg, ovl->regs,
			   DISP_REG_OVL_SRC_CON, BIT(idx));
	mtk_ddp_write(cmdq_pkt, 0, &ovl->cmdq_reg, ovl->regs,
		      DISP_REG_OVL_RDMA_CTRL(idx));
}

static unsigned int mtk_ovl_fmt_convert(struct mtk_disp_ovl *ovl, struct mtk_plane_state *state)
{
	unsigned int fmt = state->pending.format;
	unsigned int blend_mode = DRM_MODE_BLEND_COVERAGE;

	/*
 * For the platforms where OVL_CON_CLRFMT_MAN is defined in the hardware data sheet
 * and supports premultiplied color formats, such as OVL_CON_CLRFMT_PARGB8888.
 *
 * Check blend_modes in the driver data to see if premultiplied mode is supported.
 * If not, use coverage mode instead to set it to the supported color formats.
 *
 * Current DRM assumption is that alpha is default premultiplied, so the bitmask of
 * blend_modes must include BIT(DRM_MODE_BLEND_PREMULTI). Otherwise, mtk_plane_init()
 * will get an error return from drm_plane_create_blend_mode_property() and
 * state->base.pixel_blend_mode should not be used.
	 */
	if (ovl->data->blend_modes & BIT(DRM_MODE_BLEND_PREMULTI))
		blend_mode = state->base.pixel_blend_mode;

	switch (fmt) {
	default:
	case DRM_FORMAT_RGB565:
		return OVL_CON_CLRFMT_RGB565(ovl);
	case DRM_FORMAT_BGR565:
		return OVL_CON_CLRFMT_RGB565(ovl) | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_RGB888:
		return OVL_CON_CLRFMT_RGB888(ovl);
	case DRM_FORMAT_BGR888:
		return OVL_CON_CLRFMT_RGB888(ovl) | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBA1010102:
		return blend_mode == DRM_MODE_BLEND_COVERAGE ?
		       OVL_CON_CLRFMT_RGBA8888 :
		       OVL_CON_CLRFMT_PRGBA8888;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRA1010102:
		return blend_mode == DRM_MODE_BLEND_COVERAGE ?
		       OVL_CON_CLRFMT_BGRA8888 :
		       OVL_CON_CLRFMT_PBGRA8888;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		return blend_mode == DRM_MODE_BLEND_COVERAGE ?
		       OVL_CON_CLRFMT_ARGB8888 :
		       OVL_CON_CLRFMT_PARGB8888;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return blend_mode == DRM_MODE_BLEND_COVERAGE ?
		       OVL_CON_CLRFMT_ABGR8888 :
		       OVL_CON_CLRFMT_PABGR8888;
	case DRM_FORMAT_UYVY:
		return OVL_CON_CLRFMT_UYVY;
	case DRM_FORMAT_YUYV:
		return OVL_CON_CLRFMT_YUYV;
	}
}

/*
 * OVL_CON[15:12] CLRFMT, per the MT6589 data sheet:
 * 0000 RGB888, 0001 RGB565, 0010 ARGB888, 0011 PARGB8888,
 * 0100 xARGB8888, 1000 YUYV, 1001 UYVY.
 *
 * YUV to RGB conversion is implied purely by selecting a YUV CLRFMT; the
 * coefficients are programmed separately by mt6589_ovl_write_yuv_matrix().
 * There is no matrix enable bit in OVL_CON on this SoC, and OVL_CON[23:16]
 * is HORI_BLOCK_NUM, which nothing here sets.
 */
static unsigned int mt6589_fmt_convert(unsigned int fmt, unsigned int blend_mode)
{
	switch (fmt) {
	case DRM_FORMAT_RGB565:  return (1 << 12);
	case DRM_FORMAT_BGR565:  return (1 << 12) | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_RGB888:  return (0 << 12);
	case DRM_FORMAT_BGR888:  return (0 << 12) | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888: return (3 << 12);
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888: return (3 << 12) | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888: return (2 << 12);
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888: return (2 << 12) | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_UYVY:    return (9 << 12);
	case DRM_FORMAT_YUYV:    return (8 << 12);
	}
	return 0;
}

static void mtk_ovl_afbc_layer_config(struct mtk_disp_ovl *ovl,
				      unsigned int idx,
				      struct mtk_plane_pending_state *pending,
				      struct cmdq_pkt *cmdq_pkt)
{
	unsigned int pitch_msb = pending->pitch >> 16;
	unsigned int hdr_pitch = pending->hdr_pitch;
	unsigned int hdr_addr = pending->hdr_addr;

	if (pending->modifier != DRM_FORMAT_MOD_LINEAR) {
		mtk_ddp_write_relaxed(cmdq_pkt, hdr_addr, &ovl->cmdq_reg, ovl->regs,
				      DISP_REG_OVL_HDR_ADDR(ovl, idx));
		mtk_ddp_write_relaxed(cmdq_pkt,
				      OVL_PITCH_MSB_2ND_SUBBUF | pitch_msb,
				      &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_PITCH_MSB(idx));
		mtk_ddp_write_relaxed(cmdq_pkt, hdr_pitch, &ovl->cmdq_reg, ovl->regs,
				      DISP_REG_OVL_HDR_PITCH(ovl, idx));
	} else {
		mtk_ddp_write_relaxed(cmdq_pkt, pitch_msb,
				      &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_PITCH_MSB(idx));
	}
}

void mtk_ovl_layer_config(struct device *dev, unsigned int idx,
			  struct mtk_plane_state *state,
			  struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	struct mtk_plane_pending_state *pending = &state->pending;
	unsigned int addr = pending->addr;
	unsigned int pitch_lsb = pending->pitch & GENMASK(15, 0);
	unsigned int fmt = pending->format;
	unsigned int rotation = pending->rotation;
	unsigned int offset = (pending->y << 16) | pending->x;
	unsigned int src_size = (pending->height << 16) | pending->width;
	unsigned int blend_mode = state->base.pixel_blend_mode;
	unsigned int ignore_pixel_alpha = 0, const_blend = 0;
	unsigned int con;

	if (!pending->enable) {
		mtk_ovl_layer_off(dev, idx, cmdq_pkt);
		return;
	}

	if (ovl->data->fmt_convert)
		con = ovl->data->fmt_convert(fmt, blend_mode);
	else
		con = mtk_ovl_fmt_convert(ovl, state);

	if (ovl->data->has_const_blend)
		const_blend = OVL_CONST_BLEND;

	if (state->base.fb) {
		con |= state->base.alpha & OVL_CON_ALPHA;

		/*
 * For blend_modes supported SoCs, always enable alpha blending.
 * For blend_modes unsupported SoCs, enable alpha blending when has_alpha is set.
		 */
		if (blend_mode || state->base.fb->format->has_alpha)
			con |= OVL_CON_AEN;

		/*
 * Although the alpha channel can be ignored, CONST_BLD must be enabled
 * for XRGB format, otherwise OVL will still read the value from memory.
 * For RGB888 related formats, whether CONST_BLD is enabled or not won't
 * affect the result. Therefore we use !has_alpha as the condition.
		 */
		if (blend_mode == DRM_MODE_BLEND_PIXEL_NONE || !state->base.fb->format->has_alpha)
			ignore_pixel_alpha = const_blend;
	}

	/*
 * Treat rotate 180 as flip x + flip y, and XOR the original rotation value
 * to flip x + flip y to support both in the same time.
	 */
	if (rotation & DRM_MODE_ROTATE_180)
		rotation ^= DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;

	if (rotation & DRM_MODE_REFLECT_Y) {
		con |= OVL_CON_VIRT_FLIP;
		addr += (pending->height - 1) * pending->pitch;
	}

	if (rotation & DRM_MODE_REFLECT_X) {
		con |= OVL_CON_HORZ_FLIP;
		addr += pending->pitch - 1;
	}

	if (ovl->data->supports_afbc)
		mtk_ovl_set_afbc(ovl, cmdq_pkt, idx,
				 pending->modifier != DRM_FORMAT_MOD_LINEAR);

	mtk_ddp_write_relaxed(cmdq_pkt, con, &ovl->cmdq_reg, ovl->regs,
			      DISP_REG_OVL_CON(idx));
	/*
 * OVL_PITCH counts pixels per line, not bytes.  The stock driver
 * computes the layer address as
 *
 *	addr + src_x * bpp + src_y * src_pitch
 *
 * scaling src_pitch by a line index while src_x is scaled by
 * bytes-per-pixel, so the pitch can only be in pixels.  The value
 * handed to us is drm's fb->pitches[0], which is in bytes.
	 */
	pitch_lsb = (pending->pitch / pending->cpp) & GENMASK(15, 0);

	mtk_ddp_write_relaxed(cmdq_pkt, pitch_lsb | ignore_pixel_alpha,
			      &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_PITCH(idx));
	mtk_ddp_write_relaxed(cmdq_pkt, src_size, &ovl->cmdq_reg, ovl->regs,
			      DISP_REG_OVL_SRC_SIZE(idx));
	mtk_ddp_write_relaxed(cmdq_pkt, offset, &ovl->cmdq_reg, ovl->regs,
			      DISP_REG_OVL_OFFSET(idx));
	mtk_ddp_write_relaxed(cmdq_pkt, addr, &ovl->cmdq_reg, ovl->regs,
			      DISP_REG_OVL_ADDR(ovl, idx));

	if (ovl->data->supports_afbc)
		mtk_ovl_afbc_layer_config(ovl, idx, pending, cmdq_pkt);

	mtk_ovl_set_bit_depth(dev, idx, fmt, cmdq_pkt);
	mtk_ovl_layer_on(dev, idx, cmdq_pkt);

	/* For MT6589, write YUV conversion matrix if needed */
	if (ovl->data->fmt_convert == mt6589_fmt_convert &&
	    (fmt == DRM_FORMAT_UYVY || fmt == DRM_FORMAT_YUYV))
		mt6589_ovl_write_yuv_matrix(ovl, idx, cmdq_pkt);
}

void mtk_ovl_bgclr_in_on(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int reg;

	reg = readl(ovl->regs + DISP_REG_OVL_DATAPATH_CON);
	reg = reg | OVL_BGCLR_SEL_IN;
	writel(reg, ovl->regs + DISP_REG_OVL_DATAPATH_CON);
}

void mtk_ovl_bgclr_in_off(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int reg;

	reg = readl(ovl->regs + DISP_REG_OVL_DATAPATH_CON);
	reg = reg & ~OVL_BGCLR_SEL_IN;
	writel(reg, ovl->regs + DISP_REG_OVL_DATAPATH_CON);
}

static int mtk_disp_ovl_bind(struct device *dev, struct device *master,
			     void *data)
{
	return 0;
}

static void mtk_disp_ovl_unbind(struct device *dev, struct device *master,
				void *data)
{
}

static const struct component_ops mtk_disp_ovl_component_ops = {
	.bind	= mtk_disp_ovl_bind,
	.unbind = mtk_disp_ovl_unbind,
};

/*
 * Reject SoC data that would leave the interrupt path self-inconsistent.
 *
 * int_all_mask exists for one reason: whatever mtk_ovl_enable_vblank()
 * switches on in OVL_INTEN has to be acknowledged again by the ISR, or a
 * level-triggered line stays asserted and re-enters the handler forever.
 * So the enable mask must be a subset of the acknowledge mask.  Checking
 * it here is what lets the ISR treat a zero int_all_mask as a deliberate
 * value instead of "unknown": the check proves that a zero acknowledge mask
 * can only be paired with a zero enable mask, i.e. a SoC that never raises
 * an OVL interrupt through this driver at all.
 *
 * The second condition keeps a status bit that could fire vblank_cb out of
 * a SoC with no enable mask: nothing would ever enable it, so gating
 * vblank on it would be a lie, while claiming it does fire means "any
 * status bit counts", which cannot be true if there are no status bits.
 */
static int mtk_disp_ovl_check_data(struct device *dev,
				   const struct mtk_disp_ovl_data *data)
{
	if (data->vblank_en_mask & ~data->int_all_mask) {
		dev_err(dev,
			"OVL_INTEN bits %#x are not acknowledged by int_all_mask %#x\n",
			data->vblank_en_mask, data->int_all_mask);
		return -EINVAL;
	}

	if (!data->int_all_mask && data->fme_cpl_bit) {
		dev_err(dev,
			"fme_cpl_bit %#x cannot fire vblank with int_all_mask 0\n",
			data->fme_cpl_bit);
		return -EINVAL;
	}

	return 0;
}

static int mtk_disp_ovl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_ovl *priv;
	int irq;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	/*
 * The node declares both the engine and the SMI clock.  Taking only
 * index 0 left SMI gated, and without it the GMC and M4U writes this
 * driver performs may not reach the hardware.
	 */
	/*
 * Take every clock the node declares.  devm_clk_get() can only return
 * one of them: with no clock-names property it always resolves index
 * 0, so the SMI clock stayed gated.  This helper walks the whole
 * clocks property by index instead.
	 */
	ret = devm_clk_bulk_get_all(dev, &priv->clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get ovl clks\n");
	priv->num_clks = ret;

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "failed to ioremap ovl\n");

	/*
 * Reset the engine before touching it.  Writing the registers
 * directly needs the block's clock running, but the bootloader can
 * leave it in a state where the engine is still fetching, so assert
 * the reset first and release it with the clocks enabled.
	 */
	ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable ovl clks\n");

	/*
 * Look the reset up by index.  Passing a name would make the core
 * search the "reset-names" property first, and the display nodes
 * carry a bare "resets = <&dispsys MT6589_DISP_OVL_RST>" with no
 * reset-names, so a named lookup fails with -ENOENT before any reset
 * controller is ever consulted.  Every other MediaTek display driver
 * here looks its reset up this way for the same reason.
	 */
	priv->rstc = devm_reset_control_get(dev, NULL);
	if (IS_ERR(priv->rstc)) {
		ret = PTR_ERR(priv->rstc);
		clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
		return dev_err_probe(dev, ret,
				     "failed to get reset control\n");
	}

	ret = reset_control_reset(priv->rstc);
	if (ret) {
		clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
		return dev_err_probe(dev, ret, "failed to reset ovl\n");
	}

	/* Stop any leftover OVL activity from bootloader */
	writel(0x0, priv->regs + DISP_REG_OVL_EN);
	writel(0x0, priv->regs + DISP_REG_OVL_INTEN);
	writel(0x0, priv->regs + DISP_REG_OVL_INTSTA);

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "get mediatek,gce-client-reg fail!\n");
#endif

	priv->data = of_device_get_match_data(dev);

	ret = mtk_disp_ovl_check_data(dev, priv->data);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	ret = devm_request_irq(dev, irq, mtk_disp_ovl_irq_handler,
			       IRQF_TRIGGER_NONE, dev_name(dev), priv);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to request irq %d\n", irq);

	pm_runtime_enable(dev);

	ret = component_add(dev, &mtk_disp_ovl_component_ops);
	if (ret) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "Failed to add component\n");
	}

	return 0;
}

static void mtk_disp_ovl_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_ovl_component_ops);
	pm_runtime_disable(&pdev->dev);
}

static const struct mtk_disp_ovl_data mt2701_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT2701,
	.gmc_bits = 8,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = false,
	.int_all_mask = 0,	/* see the note above the match table */
	.fme_cpl_bit = OVL_FME_CPL_ANY,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8167_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 8,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.smi_id_en = true,
	.int_all_mask = 0,	/* see the note above the match table */
	.fme_cpl_bit = OVL_FME_CPL_ANY,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt6589_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT2701,
	.gmc_bits = 10,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = false,
	.blend_modes = BIT(DRM_MODE_BLEND_PREMULTI) |
		       BIT(DRM_MODE_BLEND_COVERAGE) |
		       BIT(DRM_MODE_BLEND_PIXEL_NONE),
	.has_const_blend = false,
	.set_layer_src = true,
	.vblank_en_mask = 0xF, /* Reg update, frame done, underflow, sw reset done */
	/*
	 * This is the only SoC here with a documented OVL_INTEN/OVL_INTSTA
	 * bit map, so it is the only one that can name its status bits.
	 * Bits 0, 1, 2 and 3 are enabled above, so all of them - and the
	 * whole documented map - must be acknowledged in the ISR.
	 */
	.int_all_mask = OVL_INT_ALL,
	.fme_cpl_bit = OVL_FME_CPL_INT,
	.fme_und_bit = BIT(2),
	.rdma0_eof_abn_bit = BIT(4),
	.rdma1_eof_abn_bit = BIT(5),
	.rdma0_fifo_und_bit = BIT(8),
	.rdma1_fifo_und_bit = BIT(9),
	.fmt_convert = mt6589_fmt_convert,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8173_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 8,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.int_all_mask = 0,	/* see the note above the match table */
	.fme_cpl_bit = OVL_FME_CPL_ANY,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8183_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.int_all_mask = 0,	/* see the note above the match table */
	.fme_cpl_bit = OVL_FME_CPL_ANY,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8183_ovl_2l_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 2,
	.fmt_rgb565_is_0 = true,
	.int_all_mask = 0,	/* see the note above the match table */
	.fme_cpl_bit = OVL_FME_CPL_ANY,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8192_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.smi_id_en = true,
	.blend_modes = BIT(DRM_MODE_BLEND_PREMULTI) |
		       BIT(DRM_MODE_BLEND_COVERAGE) |
		       BIT(DRM_MODE_BLEND_PIXEL_NONE),
	.int_all_mask = 0,	/* see the note above the match table */
	.fme_cpl_bit = OVL_FME_CPL_ANY,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8192_ovl_2l_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 2,
	.fmt_rgb565_is_0 = true,
	.smi_id_en = true,
	.blend_modes = BIT(DRM_MODE_BLEND_PREMULTI) |
		       BIT(DRM_MODE_BLEND_COVERAGE) |
		       BIT(DRM_MODE_BLEND_PIXEL_NONE),
	.int_all_mask = 0,	/* see the note above the match table */
	.fme_cpl_bit = OVL_FME_CPL_ANY,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8195_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.smi_id_en = true,
	.supports_afbc = true,
	.blend_modes = BIT(DRM_MODE_BLEND_PREMULTI) |
		       BIT(DRM_MODE_BLEND_COVERAGE) |
		       BIT(DRM_MODE_BLEND_PIXEL_NONE),
	.int_all_mask = 0,	/* see the note above the match table */
	.fme_cpl_bit = OVL_FME_CPL_ANY,
	.formats = mt8195_formats,
	.num_formats = ARRAY_SIZE(mt8195_formats),
	.supports_clrfmt_ext = true,
};

/*
 * The eight SoCs other than MT6589 enable no OVL interrupt at all.
 *
 * Nothing this driver does gives them one.  Every write to OVL_INTEN is
 * either data->vblank_en_mask, which is 0 for all eight and so programs
 * an empty enable mask, or a literal 0 (mtk_ovl_stop(), and the leftover
 * bootloader scrub in mtk_disp_ovl_probe()).  Their DT nodes wire the
 * OVL interrupt as a dedicated GIC line - MT8173 SPI 180, MT8183 SPI 225,
 * MT8192 SPI 254, MT8195 SPI 636 - not a shared disp_ovl interrupt, so
 * nothing else in the display stack raises it either.  The status
 * register may therefore latch bits, but nothing can raise the line and
 * the handler below is unreachable from this driver.
 *
 * That is why their values are int_all_mask 0 and fme_cpl_bit
 * OVL_FME_CPL_ANY, stated rather than defaulted:
 *
 *   - int_all_mask 0 says "there is no status bit for this driver to
 *     acknowledge", which is true, and makes the acknowledge write
 *     leave the status register untouched - which is what these SoCs
 *     have always done, and is the only behaviour their unused status
 *     bits permit.
 *   - fme_cpl_bit OVL_FME_CPL_ANY keeps the vblank gate at "some status
 *     bit was latched" rather than "never".  Were it 0 no interrupt
 *     could ever fire a vblank on these SoCs, which would silently stop
 *     any pipeline that does reach the handler for some other reason.
 *
 * Neither value is a guess about undocumented hardware: both are the
 * values that keep these SoCs doing exactly what they do today.
 */
static const struct of_device_id mtk_disp_ovl_driver_dt_match[] = {
	{ .compatible = "mediatek,mt2701-disp-ovl",
	  .data = &mt2701_ovl_driver_data},
	{ .compatible = "mediatek,mt6589-disp-ovl",
	  .data = &mt6589_ovl_driver_data},
	{ .compatible = "mediatek,mt8167-disp-ovl",
	  .data = &mt8167_ovl_driver_data},
	{ .compatible = "mediatek,mt8173-disp-ovl",
	  .data = &mt8173_ovl_driver_data},
	{ .compatible = "mediatek,mt8183-disp-ovl",
	  .data = &mt8183_ovl_driver_data},
	{ .compatible = "mediatek,mt8183-disp-ovl-2l",
	  .data = &mt8183_ovl_2l_driver_data},
	{ .compatible = "mediatek,mt8192-disp-ovl",
	  .data = &mt8192_ovl_driver_data},
	{ .compatible = "mediatek,mt8192-disp-ovl-2l",
	  .data = &mt8192_ovl_2l_driver_data},
	{ .compatible = "mediatek,mt8195-disp-ovl",
	  .data = &mt8195_ovl_driver_data},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_disp_ovl_driver_dt_match);

struct platform_driver mtk_disp_ovl_driver = {
	.probe		= mtk_disp_ovl_probe,
	.remove		= mtk_disp_ovl_remove,
	.driver		= {
		.name	= "mediatek-disp-ovl",
		.of_match_table = mtk_disp_ovl_driver_dt_match,
	},
};
