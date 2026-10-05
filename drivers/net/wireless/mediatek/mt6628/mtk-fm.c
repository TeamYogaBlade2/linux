// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6628 FM radio driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The FM receiver lives inside the MT6628 combo chip and is controlled
 * through the STP control channel (SDIO function 2) with the chip's
 * "basic operation" (BOP) command language: each BOP is a small
 * [opcode][size][args...] program executed by the on-chip firmware.
 *
 * Firmware: the DSP runs from mt6628_fm_rom.bin, optionally patched by
 * mt6628_fm_v<N>_patch.bin and calibrated with
 * mt6628_fm_v<N>_coeff.bin (all under /lib/firmware/mediatek/).
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/firmware.h>
#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6628.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

/* FM BOP opcodes */
#define FM_BOP_BASE			0x80
#define FM_BOP_WRITE			(FM_BOP_BASE + 0x00)
#define FM_BOP_UDELAY			(FM_BOP_BASE + 0x01)
#define FM_BOP_RD_UNTIL			(FM_BOP_BASE + 0x02)
#define FM_BOP_MODIFY			(FM_BOP_BASE + 0x03)
#define FM_BOP_MSLEEP			(FM_BOP_BASE + 0x04)

/* FM packet types on the STP control channel */
#define FM_TASK_COMMAND_PKT_TYPE	0x1
#define FM_TASK_EVENT_PKT_TYPE		0x4
#define FM_ENABLE_OPCODE		0x07
#define FM_FSPI_READ_OPCODE		0x03
#define FM_FSPI_WRITE_OPCODE		0x04
#define FM_TUNE_OPCODE			0x09
#define FM_SEEK_OPCODE			0x0a
#define FM_PATCH_DOWNLOAD_OPCODE	0x12
#define FM_COEFF_DOWNLOAD_OPCODE	0x13
/*
 * Unsolicited event carrying decoded RDS groups, from the opcode table in
 * aquaris-5/.../core/inc/fm_link.h:35.  The vendor link parser special-cases
 * it in fm_event_parser() (core/fm_link.c:389-409) and hands the payload
 * straight to the RDS parser, confirming it is a plain data event.
 */
#define FM_RDS_DATA_OPCODE		0x0d

#define FM_REG_CHIP_ID			0x62
#define FM_REG_ROM_VERSION		0x83
#define FM_REG_ROM_CTRL			0x61
#define FM_REG_CG_CTRL			0x60
#define FM_REG_RSSI_IND			0x6c
#define FM_REG_FORCE_MS			0x75
#define FM_FORCE_MS			0x0008
#define FM_STEREO_IND			BIT(12)
/*
 * FM_MAIN_CTRL[5] is the receiver mute bit, named MUTE in
 * aquaris-5/mediatek/kernel/drivers/fmradio/mt6628/inc/mt6628_fm_reg.h:65
 * (register 0x63 at :8).  mt6628_Mute() in pub/mt6628_fm_lib.c:212-229 sets
 * and clears it with a read-modify-write of the whole register, which is what
 * mtk_fm_set_mute() below does.
 */
#define FM_REG_MAIN_CTRL		0x63
#define FM_MUTE				0x0020

/*
 * FM_MAIN_CTRL[RDS_MASK] (bit 4) enables the RDS receiver, named RDS_MASK in
 * aquaris-5/.../mt6628/inc/mt6628_fm_reg.h:64 (the FM_MAIN_CTRL enum at :55
 * of the register list, which starts at :3).  mt6628_RDS_enable() in
 * pub/mt6628_fm_rds.c:58-70 sets it and mt6628_RDS_disable() at :72-82
 * clears it, both as a read-modify-write of the whole register.
 *
 * FM_RDS_CFG0 = 0x80.  mt6628_RDS_enable() writes the literal 6 into it,
 * commented "set buf_start_th" (pub/mt6628_fm_rds.c:65).  Nothing in the
 * vendor tree explains that threshold, so it is reproduced as the same
 * constant rather than guessed at.
 */
#define FM_RDS_MASK			0x0010
#define FM_REG_RDS_CFG0			0x80
#define FM_RDS_CFG0_BUF_START_TH	6

/*
 * FM_MAIN_CG2_CTRL[12] selects de-emphasis, named DE_EMPHASIS in
 * aquaris-5/.../mt6628/inc/mt6628_fm_reg.h:83 with the comment
 * "0x61 D12, 0:50us, 1:75 us".  mt6628_pwrup_clock_on() applies it with
 * fm_bop_modify(0x61, ~DE_EMPHASIS, de_emphasis << 12) at
 * pub/mt6628_fm_cmd.c:295.
 */
#define FM_DEEMPHASIS			0x1000

/*
 * The output volume is register 0x7D, selected through a 16-entry lookup
 * table rather than a linear field: mt6628_SetVol() in
 * aquaris-5/.../mt6628/pub/mt6628_fm_lib.c:1100-1119 clamps to 15 and writes
 * mt6628_vol_tbl[vol] (the table is at :1095-1099).  mt6628_GetVol() at
 * :1121-1143 reads the register back and searches for the matching table
 * index, which confirms the register holds the table value verbatim.
 */
#define FM_REG_VOLUME			0x7d

static const u16 mtk_fm_vol_tbl[16] = {
	0x0000, 0x0519, 0x066a, 0x0814,
	0x0a2b, 0x0ccd, 0x101d, 0x1449,
	0x198a, 0x2027, 0x287a, 0x32f5,
	0x4027, 0x50c3, 0x65ad, 0x7fff,
};

/*
 * RDS decoder state.
 *
 * The chip delivers decoded RDS groups, not a bit stream.  Each group arrives
 * in one RDS_RX_DATA_OPCODE event packet as up to MAX_RDS_RX_GROUP_CNT
 * six-word records (aquaris-5/.../inc/fm_rds.h:15-32, and the payload member
 * in core/inc/fm_link.h:94).  A record holds the four 16-bit blocks, a
 * per-group flag word ("crc") saying which blocks the demodulator accepted,
 * and a per-group correct-bit count.
 *
 * The block-error bit positions come from the vendor parser, not from the RDS
 * standard, because this demodulator reports errors its own way:
 *
 *   FM_RDS_GDBK_IND_A 0x08	inc/fm_rds.h:5
 *   FM_RDS_GDBK_IND_B 0x04	inc/fm_rds.h:6
 *   FM_RDS_GDBK_IND_C 0x02	inc/fm_rds.h:7
 *   FM_RDS_GDBK_IND_D 0x01	inc/fm_rds.h:8
 *
 * used by rds_checksum_check() at core/fm_rds_parser.c:124-133, and by
 * rds_grp_type_get() at :203-208, rds_grp_pi_get() at :331 and
 * rds_g0_ps_get() at :510.
 */
#define FM_RDS_GDBK_IND_A		0x08
#define FM_RDS_GDBK_IND_B		0x04
#define FM_RDS_GDBK_IND_C		0x02
#define FM_RDS_GDBK_IND_D		0x01

/*
 * Maximum number of RDS groups per event packet and the firmware record
 * size.  aquaris-5/.../inc/fm_rds.h:15 fixes MAX_RDS_RX_GROUP_CNT at 12 and
 * struct rds_packet_t at :19-26 is six 16-bit words, so a record is 12 bytes
 * and a full packet is 2 words of sin/cos plus 12 records.
 */
#define FM_RDS_MAX_GROUPS		12
#define FM_RDS_HDR_SIZE			4
#define FM_RDS_REC_SIZE			(6 * 2)

/* Program service name: 4 segments of 2 characters (group 0). */
#define FM_RDS_PS_SEGMENTS		4
#define FM_RDS_PS_LEN			(FM_RDS_PS_SEGMENTS * 2)

/*
 * Radio text: 16 segments.  A group 2A segment carries 4 characters and a 2B
 * segment 2, so the buffer holds the longest possible text; the vendor uses
 * idx = 4 * addr for version A and 2 * addr for version B in rds_g2_rt_get()
 * at core/fm_rds_parser.c:796-828.
 */
#define FM_RDS_RT_SEGMENTS		16
#define FM_RDS_RT_LEN			(FM_RDS_RT_SEGMENTS * 4)

/* A segment must be received identically twice before it is accepted. */
#define FM_RDS_SEG_MIN_HITS		2

/*
 * Alternative frequencies.  The vendor keeps up to 8 pairs and accepts AF_H
 * in 225..249 as the pair count and AF_L in 1..204 as the frequency offset
 * in 100 kHz from 87.5 MHz (core/fm_rds_parser.c:1021-1035).
 */
#define FM_RDS_AF_MAX			8

/* One decoded group: the four blocks plus the chip's per-block status. */
struct mtk_fm_rds_grp {
	u16 blk[4];
	u16 crc;
	u16 cbc;
};

struct mtk_fm_rds {
	u16 pi;			/* last program identifier, all of block A */

	/* Alternative frequency list, in 100 kHz units. */
	u16 af[FM_RDS_AF_MAX];
	u32 af_mask;
	unsigned int af_count;

	u8 pty;
	bool tp;
	bool ta;
	bool ms;

	/* Last published values, to raise a control event only on change. */
	u8 pub_pty;
	bool pub_ta;
	bool pub_tp;
	bool pub_ms;

	/* Program service name: 4 segments of 2 characters. */
	u8 ps_seg[FM_RDS_PS_SEGMENTS * 2];
	u8 ps_hits[FM_RDS_PS_SEGMENTS];
	u8 ps_seg_ok;
	char ps[FM_RDS_PS_LEN];

	/* Radio text: 16 segments of up to 4 characters. */
	u8 rt_seg[FM_RDS_RT_SEGMENTS * 4];
	u8 rt_hits[FM_RDS_RT_SEGMENTS];
	u16 rt_seg_ok;
	char rt[FM_RDS_RT_LEN];
	unsigned int rt_len;
};

/*
 * FM_RSSI_IND[9:0] is a signed 10-bit field whose LSB is 6/16 = 0.375 dB,
 * so the raw code is a signal level in 1/16 dB steps.
 * aquaris-5/mediatek/kernel/drivers/fmradio/mt6628/pub/mt6628_fm_lib.c:1072-1092
 * (mt6628_GetCurRSSI) masks 0x03ff and converts with
 *	(RS > 511) ? ((RS - 1024) * 6) >> 4 : (RS * 6) >> 4
 * and the same mask and 1024 bias appear in the CQI path at :1414.
 * Read as a signed code the range is -512 .. 511, i.e.
 * -3072 .. +3066 in 1/16 dB units.
 */
#define FM_RSSI_MASK			0x03ff
#define FM_RSSI_SIGN_BIT		0x0200
#define FM_RSSI_BIAS			1024
#define FM_RSSI_ONE_LSB_NUM		6	/* 1/16 dB per raw code */
#define FM_RSSI_MIN_16			(-512 * FM_RSSI_ONE_LSB_NUM)
#define FM_RSSI_SPAN_16			(1023 * FM_RSSI_ONE_LSB_NUM)

#define FM_PATCH_SEG_LEN		512
#define FM_CMD_TIMEOUT_MS		3000

struct mtk_fm {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct mt6628_wmt *wmt;
	struct completion cmd_done;
	struct mutex cmd_lock;
	struct mutex power_lock;
	struct v4l2_ctrl *rds_ps;	/* V4L2_CID_RDS_RX_PS_NAME */
	struct v4l2_ctrl *rds_rt;	/* V4L2_CID_RDS_RX_RADIO_TEXT */
	struct v4l2_ctrl *rds_pty;	/* V4L2_CID_RDS_RX_PTY */
	struct v4l2_ctrl *rds_ta;	/* ..._TRAFFIC_ANNOUNCEMENT */
	struct v4l2_ctrl *rds_tp;	/* ..._TRAFFIC_PROGRAM */
	struct v4l2_ctrl *rds_ms;	/* ..._MUSIC_SPEECH */
	struct mtk_fm_rds rds;
	bool mute;			/* cached V4L2_CID_AUDIO_MUTE value */
	bool deemph_75us;		/* cached V4L2_CID_TUNE_DEEMPHASIS */
	bool rds_on;			/* cached V4L2_CID_RDS_RECEPTION */
	unsigned int volume;		/* cached V4L2_CID_AUDIO_VOLUME */
	u8 waiting_opcode;
	int cmd_status;
	u8 cmd_data[4];
	size_t cmd_data_len;
	u32 freq;			/* in 10 kHz units */
	unsigned int users;
	bool powered;
};

/*
 * RDS decoding.
 *
 * Two rules from the vendor parser are reproduced deliberately, because a
 * naive reading of the RDS standard would get them wrong for this chip:
 *
 *  1. A block is usable only if the matching FM_RDS_GDBK_IND_x bit is set in
 *     the group's crc word (rds_checksum_check(),
 *     core/fm_rds_parser.c:124-133).  The demodulator does the error
 *     correction itself and reports only whether each block survived.
 *
 *  2. A multi-part field (PS, RT) is reported only once a segment has been
 *     received twice with identical contents.  The vendor does this with a
 *     three-deep fresh/once/twice comparison in rds_g0_ps_cmp()
 *     (core/fm_rds_parser.c:532-592) and rds_g2_rt_cmp() at :857.
 */

static void mtk_fm_rds_reset(struct mtk_fm *fm)
{
	struct mtk_fm_rds *rds = &fm->rds;

	memset(rds, 0, sizeof(*rds));
	/*
	 * PS and RT are pre-filled with spaces rather than zeroes, as the
	 * vendor does in mt6628_RDS_Init_Data() (pub/mt6628_fm_rds.c:228)
	 * and in rds_retrieve_g2() at core/fm_rds_parser.c:1465-1467.
	 */
	memset(rds->ps, ' ', sizeof(rds->ps));
	memset(rds->rt, ' ', sizeof(rds->rt));
}

/*
 * Publish a decoded string as its V4L2 control value.
 *
 * The buffer handed to the control must be NUL terminated, so rt_len is
 * clamped to leave room for the terminator: the longest possible radio text
 * fills the control's maximum exactly.  This is the same call the vivid radio
 * model uses to report received PS and radio text
 * (drivers/media/test-drivers/vivid/vivid-radio-common.c:93-94).
 */
static void mtk_fm_rds_set_ps(struct mtk_fm *fm)
{
	if (fm->rds_ps)
		v4l2_ctrl_s_ctrl_string(fm->rds_ps, fm->rds.ps);
}

static void mtk_fm_rds_set_rt(struct mtk_fm *fm)
{
	if (!fm->rds_rt)
		return;

	/*
	 * Truncate so the text plus its terminator always fit the control.
	 * strscpy() stops at the control's maximum, so an over-long text is
	 * cut rather than overflowing.
	 */
	fm->rds.rt[min_t(unsigned int, fm->rds.rt_len,
			 FM_RDS_RT_LEN - 1)] = '\0';
	v4l2_ctrl_s_ctrl_string(fm->rds_rt, fm->rds.rt);
}

/*
 * Publish the flags and identifiers that are plain integer controls.
 *
 * Each value is compared against what was last published so a control event
 * is raised only on a real change.  The vendor does the same with its "dirty"
 * flags in rds_grp_pty_get(), rds_grp_tp_get() and rds_g0_ta_get() at
 * core/fm_rds_parser.c:355-438, which raise an event only when the decoded
 * value differs from the last one.
 */
static void mtk_fm_rds_publish(struct mtk_fm *fm)
{
	struct mtk_fm_rds *rds = &fm->rds;

	if (fm->rds_pty && rds->pub_pty != rds->pty) {
		rds->pub_pty = rds->pty;
		v4l2_ctrl_s_ctrl(fm->rds_pty, rds->pty);
	}
	if (fm->rds_ta && rds->pub_ta != rds->ta) {
		rds->pub_ta = rds->ta;
		v4l2_ctrl_s_ctrl(fm->rds_ta, rds->ta);
	}
	if (fm->rds_tp && rds->pub_tp != rds->tp) {
		rds->pub_tp = rds->tp;
		v4l2_ctrl_s_ctrl(fm->rds_tp, rds->tp);
	}
	if (fm->rds_ms && rds->pub_ms != rds->ms) {
		rds->pub_ms = rds->ms;
		v4l2_ctrl_s_ctrl(fm->rds_ms, rds->ms);
	}
}

/*
 * Record one two-character PS segment.  Returns true once all four segments
 * have been received twice identically, which is the vendor's condition for
 * publishing a complete PS (the Addr_Cnt == 0x0f test at
 * core/fm_rds_parser.c:1271).
 */
static bool mtk_fm_rds_ps_segment(struct mtk_fm *fm, unsigned int seg, u8 hi,
				  u8 lo)
{
	struct mtk_fm_rds *rds = &fm->rds;

	if (seg >= FM_RDS_PS_SEGMENTS)
		return false;

	if (rds->ps_seg[seg * 2] == hi && rds->ps_seg[seg * 2 + 1] == lo) {
		rds->ps_hits[seg]++;

		/* Publish at the second identical copy, as the vendor does. */
		if (rds->ps_hits[seg] == FM_RDS_SEG_MIN_HITS) {
			rds->ps[seg * 2] = hi;
			rds->ps[seg * 2 + 1] = lo;
			rds->ps_seg_ok |= 1 << seg;
		}
	} else {
		rds->ps_hits[seg] = 1;
		rds->ps_seg[seg * 2] = hi;
		rds->ps_seg[seg * 2 + 1] = lo;
		rds->ps_seg_ok &= ~(1 << seg);
	}

	return rds->ps_seg_ok == 0xf;
}

/*
 * Record one radio text segment.  Returns true when the text is complete:
 * either a segment carried the 0x0d end marker, or every segment has been
 * received.  The vendor uses the same two conditions in rds_g2_rt_check_end()
 * and the rt_bm test at core/fm_rds_parser.c:1507-1512.
 */
static bool mtk_fm_rds_rt_segment(struct mtk_fm *fm, unsigned int seg,
				   unsigned int seg_len, const u8 *chars)
{
	struct mtk_fm_rds *rds = &fm->rds;
	unsigned int i;
	unsigned int pos = seg * 4;
	bool same = true;

	if (seg >= FM_RDS_RT_SEGMENTS)
		return false;

	/*
	 * Compare the whole segment against what was last seen for it.  The
	 * vendor compares a whole segment too (rds_g2_rt_cmp() at
	 * core/fm_rds_parser.c:857), counting a segment as good only once it
	 * has arrived identically twice.
	 */
	for (i = 0; i < seg_len; i++)
		if (rds->rt_seg[pos + i] != chars[i]) {
			same = false;
			break;
		}

	if (same) {
		rds->rt_hits[seg]++;
	} else {
		rds->rt_hits[seg] = 1;
		memcpy(&rds->rt_seg[pos], chars, seg_len);
		rds->rt_seg_ok &= ~(1 << seg);
	}

	if (rds->rt_hits[seg] < FM_RDS_SEG_MIN_HITS)
		return false;

	/* Confirmed: publish the segment. */
	memcpy(&rds->rt[pos], chars, seg_len);
	rds->rt_seg_ok |= 1 << seg;

	/*
	 * 0x0d terminates the text.  The vendor searches the whole segment
	 * for it in rds_g2_rt_check_end()
	 * (core/fm_rds_parser.c, called at :1499).
	 */
	for (i = 0; i < seg_len; i++) {
		if (chars[i] == 0x0d) {
			/*
			 * The marker itself is not part of the text, so the
			 * length stops in front of it.
			 */
			rds->rt_len = pos + i;
			return true;
		}
	}

	/* No end marker: complete only once every segment is confirmed. */
	if (rds->rt_seg_ok == 0xffff)
		rds->rt_len = FM_RDS_RT_LEN;

	return rds->rt_seg_ok == 0xffff;
}

/*
 * Collect the alternative frequency list from group 0A.
 *
 * The vendor accepts AF_H in 225..249 as the number of pairs and AF_L in
 * 1..204 as the frequency offset in 100 kHz from 87.5 MHz
 * (core/fm_rds_parser.c:1021-1035), adding 875 to convert to the 100 kHz
 * units its frequency API uses.
 *
 * V4L2 has no control for the AF list in this tree's UAPI, so the list is
 * decoded and kept but not published.  It is filled in so that a later
 * addition of a control does not have to re-derive it.
 */
static void mtk_fm_rds_af(struct mtk_fm *fm, u16 blk_c)
{
	struct mtk_fm_rds *rds = &fm->rds;
	unsigned int count = (blk_c >> 8) & 0xff;
	unsigned int freq = blk_c & 0xff;
	unsigned int i;

	/* Valid pair counts are 225..249. */
	if (count < 225 || count > 249)
		return;

	count -= 224;

	/* Valid VHF offsets are 1..204, i.e. 87.6..102.0 MHz. */
	if (freq < 1 || freq > 204)
		return;

	/* 87.5 MHz base in 100 kHz units plus the offset. */
	freq += 875;
	if (freq > 1080)
		return;

	for (i = 0; i < count && i < FM_RDS_AF_MAX; i++) {
		if (rds->af_mask & (1 << i))
			continue;

		rds->af[i] = freq;
		rds->af_mask |= 1 << i;
		rds->af_count++;
	}
}

/*
 * Decode one RDS group.
 *
 * Block usage follows rds_parser() in aquaris-5/.../core/fm_rds_parser.c:
 * 1802-1908.  Field bit positions, all read out of block B:
 *
 *   group type		blkB[15:12]	core/fm_rds_parser.c:212
 *   version B flag	blkB[11]	core/fm_rds_parser.c:213
 *   TP			blkB[10]	core/fm_rds_parser.c:404-406
 *   PTY			blkB[9:5]	core/fm_rds_parser.c:371-373
 *   TA			blkB[4]		core/fm_rds_parser.c:429-430
 *   music/speech	blkB[3]		core/fm_rds_parser.c:453-454
 *   PS segment		blkB[1:0]	core/fm_rds_parser.c:472
 *   RT segment		blkB[3:0]	core/fm_rds_parser.c:747
 *
 * Group types other than 0 and 2 carry nothing this driver surfaces; the
 * vendor's TMC (14), EON (15) and paging decoders are not ported.
 */
static void mtk_fm_rds_group(struct mtk_fm *fm, const struct mtk_fm_rds_grp *g)
{
	struct mtk_fm_rds *rds = &fm->rds;
	u16 blk_b = g->blk[1];
	u16 blk_c = g->blk[2];
	u16 blk_d = g->blk[3];
	unsigned int type;
	bool version_b;

	/* Without a usable block B there is no group type, so nothing to do. */
	if (!(g->crc & FM_RDS_GDBK_IND_B))
		return;

	type = (blk_b >> 12) & 0xf;
	version_b = blk_b & BIT(11);

	switch (type) {
	case 0:			/* PS, TA, AF, TMC */
		if (!(g->crc & FM_RDS_GDBK_IND_A))
			return;

		/* PI, including the country and programme bits, is all of block A. */
		rds->pi = g->blk[0];

		rds->ta = blk_b & BIT(4);
		rds->ms = blk_b & BIT(3);
		rds->pty = (blk_b >> 5) & 0x1f;
		rds->tp = blk_b & BIT(10);

		if (!(g->crc & FM_RDS_GDBK_IND_D))
			return;

		/* PS is two characters in block D, at segment blkB[1:0]. */
		if (mtk_fm_rds_ps_segment(fm, blk_b & 0x3, blk_d >> 8,
					  blk_d & 0xff))
			mtk_fm_rds_set_ps(fm);

		/* The AF list is in block C of the version A form only. */
		if (!version_b && (g->crc & FM_RDS_GDBK_IND_C))
			mtk_fm_rds_af(fm, blk_c);
		break;
	case 2:			/* radio text */
		if (version_b) {
			/* 2B: block D only, two characters. */
			if (!(g->crc & FM_RDS_GDBK_IND_D))
				return;

			if (mtk_fm_rds_rt_segment(fm, blk_b & 0xf, 2,
						  (const u8 *)&blk_d))
				mtk_fm_rds_set_rt(fm);
		} else {
			/* 2A: blocks C and D, four characters. */
			if (!(g->crc & (FM_RDS_GDBK_IND_C | FM_RDS_GDBK_IND_D)))
				return;

			if (mtk_fm_rds_rt_segment(fm, blk_b & 0xf, 4,
						  (const u8 *)&blk_c))
				mtk_fm_rds_set_rt(fm);
		}
		break;
	default:
		break;
	}
}

/*
 * Parse one RDS_RX_DATA_OPCODE event payload.
 *
 * The payload is the vendor struct rds_rx_t: two 16-bit words of sin/cos
 * followed by a run of twelve-byte group records (aquaris-5/.../inc/fm_rds.h:
 * 19-32).  The firmware writes it as little-endian 16-bit values, the same
 * order the STP event headers use.  The vendor's rds_cnt_get() at
 * core/fm_rds_parser.c:73-85 derives the group count by subtracting the
 * 4-byte header and dividing by the 12-byte record, which is done here too.
 *
 * This runs in the STP receive workqueue, so it must not sleep.
 */
static void mtk_fm_rds_parse(struct mtk_fm *fm, const u8 *buf, size_t len)
{
	size_t groups;
	size_t i;

	if (len < FM_RDS_HDR_SIZE)
		return;

	groups = (len - FM_RDS_HDR_SIZE) / FM_RDS_REC_SIZE;
	if (groups > FM_RDS_MAX_GROUPS)
		groups = FM_RDS_MAX_GROUPS;

	for (i = 0; i < groups; i++) {
		const u8 *p = buf + FM_RDS_HDR_SIZE + i * FM_RDS_REC_SIZE;
		struct mtk_fm_rds_grp g;

		/*
		 * Record order is blkA, blkB, blkC, blkD, crc, cbc: the
		 * vendor's rds_grp_get() at core/fm_rds_parser.c:90-117
		 * returns them in exactly that order into dst[0..5].
		 */
		g.blk[0] = get_unaligned_le16(p);
		g.blk[1] = get_unaligned_le16(p + 2);
		g.blk[2] = get_unaligned_le16(p + 4);
		g.blk[3] = get_unaligned_le16(p + 6);
		g.crc = get_unaligned_le16(p + 8);
		g.cbc = get_unaligned_le16(p + 10);

		mtk_fm_rds_group(fm, &g);
	}

	mtk_fm_rds_publish(fm);
}

static void mtk_fm_rx(void *priv, const u8 *buf, size_t len)
{
	struct mtk_fm *fm = priv;
	u16 payload_len;
	bool powered;

	if (len < 4 || buf[0] != FM_TASK_EVENT_PKT_TYPE)
		return;

	payload_len = get_unaligned_le16(buf + 2);
	if (payload_len != len - 4)
		return;

	/*
	 * RDS groups arrive unsolicited, as their own event packet, whenever
	 * the demodulator has decoded something.  They are not the answer to
	 * any command, so they are handled before the waiting-opcode check and
	 * never complete a command.
	 *
	 * The power state is re-checked here because a packet can still be in
	 * flight when the radio is closed.
	 */
	if (buf[1] == FM_RDS_DATA_OPCODE) {
		mutex_lock(&fm->power_lock);
		powered = fm->powered && fm->rds_on;
		mutex_unlock(&fm->power_lock);

		if (!powered)
			return;

		mtk_fm_rds_parse(fm, buf + 4, payload_len);
		return;
	}

	if (buf[1] != fm->waiting_opcode)
		return;

	fm->cmd_data_len = min_t(size_t, payload_len,
				 sizeof(fm->cmd_data));
	memcpy(fm->cmd_data, buf + 4, fm->cmd_data_len);
	fm->cmd_status = 0;
	complete(&fm->cmd_done);
}

static int mtk_fm_send_cmd(struct mtk_fm *fm, const u8 *buf, size_t len,
			   u8 opcode, unsigned int timeout_ms)
{
	unsigned long timeout;
	int ret;

	mutex_lock(&fm->cmd_lock);
	reinit_completion(&fm->cmd_done);
	fm->waiting_opcode = opcode;
	fm->cmd_status = -ETIMEDOUT;
	fm->cmd_data_len = 0;

	ret = mt6628_stp_send(fm->wmt, MT6628_STP_TASK_FM, buf, len);
	if (ret < 0)
		goto out;

	timeout = wait_for_completion_timeout(&fm->cmd_done,
					     msecs_to_jiffies(timeout_ms));
	if (!timeout) {
		ret = -ETIMEDOUT;
		goto out;
	}

	ret = fm->cmd_status;
out:
	fm->waiting_opcode = 0xff;
	mutex_unlock(&fm->cmd_lock);
	return ret;
}

/* ---- BOP buffer construction ---- */

static int fm_bop_write(u8 addr, u16 val, u8 *buf, int size)
{
	if (size < 5)
		return -1;

	buf[0] = FM_BOP_WRITE;
	buf[1] = 3;
	buf[2] = addr;
	buf[3] = val & 0xff;
	buf[4] = val >> 8;

	return 5;
}

static int fm_bop_modify(u8 addr, u16 mask_and, u16 mask_or,
			 u8 *buf, int size)
{
	if (size < 7)
		return -1;

	buf[0] = FM_BOP_MODIFY;
	buf[1] = 5;
	buf[2] = addr;
	buf[3] = mask_and & 0xff;
	buf[4] = mask_and >> 8;
	buf[5] = mask_or & 0xff;
	buf[6] = mask_or >> 8;

	return 7;
}

static int fm_bop_udelay(u32 us, u8 *buf, int size)
{
	if (size < 6)
		return -1;

	buf[0] = FM_BOP_UDELAY;
	buf[1] = 4;
	buf[2] = us & 0xff;
	buf[3] = (us >> 8) & 0xff;
	buf[4] = (us >> 16) & 0xff;
	buf[5] = (us >> 24) & 0xff;

	return 6;
}

static int fm_bop_rd_until(u8 addr, u16 mask, u16 value,
			   u8 *buf, int size)
{
	if (size < 7)
		return -1;

	buf[0] = FM_BOP_RD_UNTIL;
	buf[1] = 5;
	buf[2] = addr;
	buf[3] = mask & 0xff;
	buf[4] = mask >> 8;
	buf[5] = value & 0xff;
	buf[6] = value >> 8;

	return 7;
}

static int mtk_fm_read_reg(struct mtk_fm *fm, u8 addr, u16 *value)
{
	u8 cmd[5] = {
		FM_TASK_COMMAND_PKT_TYPE,
		FM_FSPI_READ_OPCODE,
		0x01, 0x00, addr,
	};
	int ret;

	ret = mtk_fm_send_cmd(fm, cmd, sizeof(cmd),
			      FM_FSPI_READ_OPCODE, FM_CMD_TIMEOUT_MS);
	if (ret)
		return ret;

	if (fm->cmd_data_len < 2)
		return -EPROTO;

	*value = get_unaligned_le16(fm->cmd_data);
	return 0;
}

static int mtk_fm_write_reg(struct mtk_fm *fm, u8 addr, u16 value)
{
	u8 cmd[7] = {
		FM_TASK_COMMAND_PKT_TYPE,
		FM_FSPI_WRITE_OPCODE,
		0x03, 0x00,
		addr,
		value & 0xff,
		value >> 8,
	};

	return mtk_fm_send_cmd(fm, cmd, sizeof(cmd),
			       FM_FSPI_WRITE_OPCODE, FM_CMD_TIMEOUT_MS);
}

/*
 * Set or clear FM_MAIN_CTRL[MUTE].
 *
 * This is a plain read-modify-write of the whole register, matching the
 * downstream mt6628_Mute() (aquaris-5/.../mt6628/pub/mt6628_fm_lib.c:212-229).
 * The surrounding bits must be preserved: the tune and seek paths of this
 * driver reuse bits [1:0] of the same register for TUNE and SEEK.  Those
 * paths use a read-modify-write that keeps bits 15:3, so a mute set here is
 * not cleared by a later tune or seek.
 */
static int mtk_fm_set_mute(struct mtk_fm *fm, bool mute)
{
	u16 val;
	int ret;

	ret = mtk_fm_read_reg(fm, FM_REG_MAIN_CTRL, &val);
	if (ret)
		return ret;

	if (mute)
		val |= FM_MUTE;
	else
		val &= ~FM_MUTE;

	return mtk_fm_write_reg(fm, FM_REG_MAIN_CTRL, val);
}

/*
 * Enable or disable the RDS receiver by setting or clearing
 * FM_MAIN_CTRL[RDS_MASK], exactly as mt6628_RDS_enable() and
 * mt6628_RDS_disable() do in aquaris-5/.../pub/mt6628_fm_rds.c:58-82.
 *
 * mt6628_RDS_enable() additionally writes the buf_start_th constant into
 * FM_RDS_CFG0 before setting the mask.  The vendor applies that only on the
 * enable edge (mt6628_RDS_OnOff() at pub/mt6628_fm_rds.c:233-250 also resets
 * the decoder state there), so it is done on enable only.
 */
static int mtk_fm_set_rds(struct mtk_fm *fm, bool enable)
{
	u16 val;
	int ret;

	if (enable) {
		ret = mtk_fm_write_reg(fm, FM_REG_RDS_CFG0,
				       FM_RDS_CFG0_BUF_START_TH);
		if (ret)
			return ret;
	}

	ret = mtk_fm_read_reg(fm, FM_REG_MAIN_CTRL, &val);
	if (ret)
		return ret;

	if (enable)
		val |= FM_RDS_MASK;
	else
		val &= ~FM_RDS_MASK;

	return mtk_fm_write_reg(fm, FM_REG_MAIN_CTRL, val);
}

/*
 * Set the output volume.  The hardware has no linear gain field: the level is
 * chosen by index into the same 16-entry table the vendor applies, so a value
 * above 15 is clamped rather than rejected, matching the "vol = (vol > 15) ?
 * 15 : vol" in mt6628_SetVol() (pub/mt6628_fm_lib.c:1103).
 */
static int mtk_fm_set_volume(struct mtk_fm *fm, unsigned int vol)
{
	return mtk_fm_write_reg(fm, FM_REG_VOLUME,
				mtk_fm_vol_tbl[min_t(unsigned int, vol,
						     ARRAY_SIZE(mtk_fm_vol_tbl) - 1)]);
}

/*
 * Set the de-emphasis time constant.  FM_MAIN_CG2_CTRL[12] selects 50 us (0)
 * or 75 us (1); see the FM_DEEMPHASIS comment above for the vendor source.
 *
 * The vendor tree writes this bit only as part of the power-up sequence
 * (pub/mt6628_fm_cmd.c:295) and has no runtime setter, so this read-modify-
 * write is a small extension of the vendor sequence rather than a port of
 * one, and has not been validated on hardware.  Only bit 12 is touched, so the
 * other FM_MAIN_CG2_CTRL fields — including the antenna type and the
 * analog/I2S select programmed during power-up — are preserved.
 */
static int mtk_fm_set_deemphasis(struct mtk_fm *fm, bool deemph_75us)
{
	u16 val;
	int ret;

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_CTRL, &val);
	if (ret)
		return ret;

	if (deemph_75us)
		val |= FM_DEEMPHASIS;
	else
		val &= ~FM_DEEMPHASIS;

	return mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
}

/*
 * Convert a raw FM_RSSI_IND register value into the V4L2 tuner->signal
 * range of 0 .. 65535.
 *
 * The downstream driver returns the signed value produced by
 * mt6628_GetCurRSSI() to userspace unchanged
 * (aquaris-5/mediatek/platform/mt6589/external/meta/fm/meta_fm.c:1000-1009
 * copies it straight into the signal level field), so there is no vendor
 * 0 .. 65535 mapping to reproduce and no calibration constant that would
 * let the value be reported in dBuV.  The mapping below is therefore a
 * plain linear scale across the full range the register can express,
 * with 0 at the noise floor and 65535 at the strongest signal.
 */
static u16 mtk_fm_rssi_to_signal(u16 rssi_ind)
{
	s32 rssi_16;

	rssi_ind &= FM_RSSI_MASK;

	/* Sign extend bits 9:0, then scale to 1/16 dB units. */
	if (rssi_ind & FM_RSSI_SIGN_BIT)
		rssi_16 = ((s32)rssi_ind - FM_RSSI_BIAS) * FM_RSSI_ONE_LSB_NUM;
	else
		rssi_16 = (s32)rssi_ind * FM_RSSI_ONE_LSB_NUM;

	return div_u64((u64)(rssi_16 - FM_RSSI_MIN_16) * U16_MAX,
		       FM_RSSI_SPAN_16);
}

static int mtk_fm_download(struct mtk_fm *fm, u8 opcode,
			   const char *name)
{
	const struct firmware *fw;
	u8 *cmd;
	unsigned int seg_num, seg_id;
	size_t offset = 0;
	int ret;

	ret = request_firmware(&fw, name, fm->dev);
	if (ret)
		return ret;

	seg_num = DIV_ROUND_UP(fw->size, FM_PATCH_SEG_LEN);
	if (!seg_num || seg_num > U8_MAX) {
		ret = -EFBIG;
		goto out_release;
	}

	cmd = kmalloc(4 + 2 + FM_PATCH_SEG_LEN, GFP_KERNEL);
	if (!cmd) {
		ret = -ENOMEM;
		goto out_release;
	}

	for (seg_id = 0; seg_id < seg_num; seg_id++) {
		size_t seg_len = min_t(size_t, FM_PATCH_SEG_LEN,
				       fw->size - offset);

		cmd[0] = FM_TASK_COMMAND_PKT_TYPE;
		cmd[1] = opcode;
		put_unaligned_le16(seg_len + 2, cmd + 2);
		cmd[4] = seg_num;
		cmd[5] = seg_id;
		memcpy(cmd + 6, fw->data + offset, seg_len);

		ret = mtk_fm_send_cmd(fm, cmd, 6 + seg_len,
				      opcode, FM_CMD_TIMEOUT_MS);
		if (ret)
			break;

		offset += seg_len;
	}

	kfree(cmd);
out_release:
	release_firmware(fw);
	return ret;
}

static int mtk_fm_download_versioned(struct mtk_fm *fm, u8 opcode,
				     unsigned int rom, const char *kind)
{
	const struct firmware *fw;
	char name[64];
	unsigned int version;
	int ret;

	/*
	 * Match the downstream selection policy:
	 * use the ROM-specific image when present, otherwise fall back
	 * to the newest available image.
	 */
	version = rom + 1;
	snprintf(name, sizeof(name),
		 "mediatek/mt6628/mt6628_fm_v%u_%s.bin",
		 version, kind);

	ret = request_firmware(&fw, name, fm->dev);
	if (!ret) {
		release_firmware(fw);
		return mtk_fm_download(fm, opcode, name);
	}

	if (ret != -ENOENT)
		return ret;

	for (version = 5; version > 0; version--) {
		if (version == rom + 1)
			continue;

		snprintf(name, sizeof(name),
			 "mediatek/mt6628/mt6628_fm_v%u_%s.bin",
			 version, kind);

		ret = request_firmware(&fw, name, fm->dev);
		if (!ret) {
			release_firmware(fw);
			dev_warn(fm->dev,
				 "ROM v%u %s firmware missing, using v%u\n",
				 rom + 1, kind, version);
			return mtk_fm_download(fm, opcode, name);
		}

		if (ret != -ENOENT)
			return ret;
	}

	return -ENOENT;
}

static int mtk_fm_get_rom_version(struct mtk_fm *fm, u8 *rom)
{
	u16 val;
	int ret;

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_CTRL, &val);
	if (ret)
		return ret;

	val |= BIT(15);
	ret = mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
	if (ret)
		return ret;

	val |= BIT(1);
	val &= ~BIT(0);
	ret = mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
	if (ret)
		return ret;

	udelay(1000);

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_VERSION, &val);
	if (ret)
		return ret;

	*rom = val >> 8;

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_CTRL, &val);
	if (ret)
		return ret;

	val &= ~BIT(15);
	ret = mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
	if (ret)
		return ret;

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_CTRL, &val);
	if (ret)
		return ret;

	val &= ~0x3;
	val |= 0x1;

	return mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
}

/* Power-up step 1: enable the FM digital clock. */
static int fm_pwrup_clock_on(struct mtk_fm *fm, u8 *buf, int bufsize)
{
	/* Match the MT6628 downstream defaults. */
	const u16 de_emphasis = 0;
	const u16 osc_freq = 0;
	int pkt = 4;

	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_ENABLE_OPCODE;
	buf[2] = 0;			/* length filled in at the end */
	buf[3] = 0;

	pkt += fm_bop_write(0x60, 0x0000, buf + pkt, bufsize - pkt);
	pkt += fm_bop_write(0x60, 0x0001, buf + pkt, bufsize - pkt);
	pkt += fm_bop_udelay(3000, buf + pkt, bufsize - pkt);
	pkt += fm_bop_write(0x60, 0x0003, buf + pkt, bufsize - pkt);
	pkt += fm_bop_write(0x60, 0x0007, buf + pkt, bufsize - pkt);
	pkt += fm_bop_modify(0x70, 0xffbf, 0x0040, buf + pkt, bufsize - pkt);
	/* no low-power mode, analog line-in, long antenna */
	pkt += fm_bop_modify(0x61, 0xff63, 0x0000, buf + pkt, bufsize - pkt);
	pkt += fm_bop_modify(0x61, 0xefff, de_emphasis << 12,
			     buf + pkt, bufsize - pkt);
	pkt += fm_bop_modify(0x60, 0xff8f, osc_freq << 4,
			     buf + pkt, bufsize - pkt);

	/* payload length for the packet header */
	buf[2] = (pkt - 4) & 0xff;
	buf[3] = (pkt - 4) >> 8;

	return pkt;
}

static int mtk_fm_power_up(struct mtk_fm *fm)
{
	u8 *buf;
	u16 chip_id;
	u8 rom;
	int pkt, ret;

	buf = kzalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	pkt = fm_pwrup_clock_on(fm, buf, 512);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_ENABLE_OPCODE, 3000);
	if (ret)
		goto out_free;

	ret = mtk_fm_read_reg(fm, FM_REG_CHIP_ID, &chip_id);
	if (ret)
		goto out_free;
	if (chip_id != 0x6628) {
		ret = -ENODEV;
		goto out_free;
	}

	ret = mtk_fm_get_rom_version(fm, &rom);
	if (ret)
		goto out_free;
	if (rom >= 5) {
		ret = -EINVAL;
		goto out_free;
	}

	ret = mtk_fm_download_versioned(fm, FM_PATCH_DOWNLOAD_OPCODE,
					rom, "patch");
	if (ret)
		goto out_free;

	ret = mtk_fm_download_versioned(fm, FM_COEFF_DOWNLOAD_OPCODE,
					rom, "coeff");
	if (ret)
		goto out_free;

	ret = mtk_fm_write_reg(fm, 0x90, 0x0040);
	if (ret)
		goto out_free;

	ret = mtk_fm_write_reg(fm, 0x90, 0x0000);
	if (ret)
		goto out_free;

	pkt = 4;
	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_ENABLE_OPCODE;
	buf[2] = 0;
	buf[3] = 0;

	pkt += fm_bop_write(0x6a, 0x2100, buf + pkt, 512 - pkt);
	pkt += fm_bop_write(0x6b, 0x2100, buf + pkt, 512 - pkt);
	pkt += fm_bop_modify(0x60, 0xfff7, 0x0008,
			     buf + pkt, 512 - pkt);
	pkt += fm_bop_modify(0x61, 0xfffd, 0x0002,
			     buf + pkt, 512 - pkt);
	pkt += fm_bop_modify(0x61, 0xfffe, 0x0000,
			     buf + pkt, 512 - pkt);
	pkt += fm_bop_udelay(200000, buf + pkt, 512 - pkt);
	pkt += fm_bop_rd_until(0x64, 0x001f, 0x0002,
			       buf + pkt, 512 - pkt);

	put_unaligned_le16(pkt - 4, buf + 2);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_ENABLE_OPCODE, 5000);
	if (!ret)
		fm->freq = 8750;

out_free:
	kfree(buf);
	return ret;
}

static int mtk_fm_power_down(struct mtk_fm *fm)
{
	u8 buf[128] = {};
	int pkt = 4;
	int ret;

	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_ENABLE_OPCODE;

	/* Disable HW clock control. */
	pkt += fm_bop_write(0x60, 0x330f,
			    buf + pkt, sizeof(buf) - pkt);

	/* Reset ASIP. */
	pkt += fm_bop_write(0x61, 0x0001,
			    buf + pkt, sizeof(buf) - pkt);

	/* Reset the digital core and digital RGF. */
	pkt += fm_bop_modify(0x6e, 0xfff8, 0x0000,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x6e, 0xfff8, 0x0000,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x6e, 0xfff8, 0x0000,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x6e, 0xfff8, 0x0000,
			     buf + pkt, sizeof(buf) - pkt);

	/* Disable all clocks and reset RGF/RF. */
	pkt += fm_bop_write(0x60, 0x0000,
			    buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_write(0x60, 0x4000,
			    buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_write(0x60, 0x0000,
			    buf + pkt, sizeof(buf) - pkt);

	put_unaligned_le16(pkt - 4, buf + 2);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_ENABLE_OPCODE, 3000);
	return ret;
}

static int mtk_fm_power_get(struct mtk_fm *fm)
{
	int ret;

	mutex_lock(&fm->power_lock);

	if (fm->users) {
		fm->users++;
		mutex_unlock(&fm->power_lock);
		return 0;
	}

	ret = mt6628_wmt_func_ctrl(fm->wmt, MT6628_WMT_FUNC_FM, true);
	if (ret)
		goto out_unlock;

	ret = mtk_fm_power_up(fm);
	if (ret) {
		mtk_fm_power_down(fm);
		mt6628_wmt_func_ctrl(fm->wmt,
				     MT6628_WMT_FUNC_FM, false);
		goto out_unlock;
	}

	fm->powered = true;
	fm->users = 1;
	ret = 0;

out_unlock:
	mutex_unlock(&fm->power_lock);
	return ret;
}

static void mtk_fm_power_put(struct mtk_fm *fm)
{
	int ret;

	mutex_lock(&fm->power_lock);

	if (!fm->users) {
		mutex_unlock(&fm->power_lock);
		return;
	}

	fm->users--;
	if (fm->users) {
		mutex_unlock(&fm->power_lock);
		return;
	}

	if (fm->powered) {
		ret = mtk_fm_power_down(fm);
		if (ret)
			dev_warn(fm->dev,
				 "FM power-down failed: %d\n", ret);
		fm->powered = false;
	}

	ret = mt6628_wmt_func_ctrl(fm->wmt,
				   MT6628_WMT_FUNC_FM, false);
	if (ret)
		dev_warn(fm->dev,
			 "FM function-off failed: %d\n", ret);

	mutex_unlock(&fm->power_lock);
}

/* Log a non-fatal restore failure and report whether it failed. */
static int mtk_fm_warn_or_ret(struct mtk_fm *fm, int ret, const char *what)
{
	if (ret)
		dev_warn(fm->dev, "failed to restore FM %s: %d\n", what, ret);

	return ret;
}

static int mtk_fm_open(struct file *file)
{
	struct mtk_fm *fm = video_drvdata(file);
	int ret;

	ret = nonseekable_open(file_inode(file), file);
	if (ret)
		return ret;

	ret = mtk_fm_power_get(fm);
	if (ret)
		return ret;

	/*
	 * The power-up sequence leaves FM_MAIN_CTRL[MUTE] clear and programs
	 * the default de-emphasis, so the hardware has to be told about any
	 * state requested before this open.  Re-applying it here also restores
	 * that state across a power cycle.  Each write touches only its own
	 * field, so an in-flight tune or seek is not disturbed.
	 *
	 * A failure here is not fatal to the open: the radio is usable without
	 * the requested volume or mute, so the state is logged and the open
	 * succeeds.  Only the power reference taken above is released if the
	 * radio was never brought up at all.
	 */
	if (fm->mute)
		mtk_fm_warn_or_ret(fm, mtk_fm_set_mute(fm, true), "mute");

	if (mtk_fm_set_volume(fm, fm->volume))
		dev_warn(fm->dev, "failed to restore FM volume: %u\n", fm->volume);

	if (fm->deemph_75us)
		mtk_fm_warn_or_ret(fm, mtk_fm_set_deemphasis(fm, true),
				   "de-emphasis");

	/*
	 * RDS is re-enabled per open, because FM_MAIN_CTRL is a power-up
	 * register and the mask is cleared by the reset the power-down
	 * sequence performs.  The decoder state is cleared with it.
	 */
	if (fm->rds_on) {
		if (!mtk_fm_set_rds(fm, true))
			mtk_fm_rds_reset(fm);
		else
			dev_warn(fm->dev, "failed to enable FM RDS\n");
	}

	return 0;
}

static int mtk_fm_release(struct file *file)
{
	struct mtk_fm *fm = video_drvdata(file);

	mtk_fm_power_put(fm);
	return 0;
}

struct mtk_fm_chan_para {
	u16 freq;
	u8 value;
};

/*
 * MT6628's downstream chan_para_map is indexed in 5 kHz units.
 * Keep only the non-zero entries here; all other entries are zero.
 */
static const struct mtk_fm_chan_para mtk_fm_chan_para_map[] = {
	{  7680, 1 },
	{  7690, 1 },
	{  8000, 8 },
	{  8210, 1 },
	{  8450, 1 },
	{  8460, 1 },
	{  8470, 1 },
	{  9210, 1 },
	{  9220, 1 },
	{  9230, 1 },
	{  9450, 1 },
	{  9460, 1 },
	{  9470, 1 },
	{  9480, 1 },
	{  9500, 1 },
	{  9510, 1 },
	{  9520, 1 },
	{  9550, 2 },
	{  9590, 1 },
	{  9600, 8 },
	{  9840, 2 },
	{  9980, 1 },
	{  9990, 1 },
	{ 10030, 1 },
	{ 10260, 2 },
	{ 10400, 8 },
	{ 10500, 1 },
	{ 10510, 1 },
	{ 10520, 1 },
	{ 10750, 1 },
	{ 10760, 1 },
	{ 10770, 1 },
};

static const u16 mtk_fm_mcu_desense_channels[] = {
	7630, 7800, 7940, 8320, 9260,
	9600, 9710, 9920, 10400, 10410,
};

static const u16 mtk_fm_gps_desense_channels[] = {
	7850, 7860,
};

static u8 mtk_fm_get_chan_para(u32 freq)
{
	unsigned int i;

	if (freq < 7600 || freq > 10800)
		return 0;

	freq = 7600 + DIV_ROUND_CLOSEST(freq - 7600, 5) * 5;

	for (i = 0; i < ARRAY_SIZE(mtk_fm_chan_para_map); i++)
		if (mtk_fm_chan_para_map[i].freq == freq)
			return mtk_fm_chan_para_map[i].value;

	return 0;
}

static bool mtk_fm_freq_in_list(u32 freq, const u16 *list, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (list[i] == freq)
			return true;

	return false;
}

static void mtk_fm_update_desense(struct mtk_fm *fm, u32 freq)
{
	int ret;

	ret = mt6628_wmt_dsns_ctrl(
		fm->wmt,
		mtk_fm_freq_in_list(freq, mtk_fm_mcu_desense_channels,
				    ARRAY_SIZE(mtk_fm_mcu_desense_channels)) ?
		MT6628_WMT_DSNS_FM_ENABLE :
		MT6628_WMT_DSNS_FM_DISABLE);
	if (ret)
		dev_warn(fm->dev, "failed to update FM MCU desense: %d\n", ret);

	ret = mt6628_wmt_dsns_ctrl(
		fm->wmt,
		mtk_fm_freq_in_list(freq, mtk_fm_gps_desense_channels,
				    ARRAY_SIZE(mtk_fm_gps_desense_channels)) ?
		MT6628_WMT_DSNS_FM_GPS_ENABLE :
		MT6628_WMT_DSNS_FM_GPS_DISABLE);
	if (ret)
		dev_warn(fm->dev, "failed to update FM GPS desense: %d\n", ret);
}

static int mtk_fm_tune(struct mtk_fm *fm, u32 freq)
{
	u8 buf[64] = {};
	u16 tune_value;
	u8 chan_para;
	int pkt = 4;
	int ret;

	if (freq < 7600 || freq > 10800)
		return -EINVAL;

	mtk_fm_update_desense(fm, freq);

	tune_value = (freq - 6400) * 2 / 10;
	chan_para = mtk_fm_get_chan_para(freq);

	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_TUNE_OPCODE;

	/*
	 * FM_CHANNEL_SET = 0x65
	 * [9:0]  desired channel
	 * [15:12] ATJ/HL/FA channel parameters
	 */
	pkt += fm_bop_modify(0x65, 0xfc00, tune_value,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x65, 0x0fff, chan_para << 12,
			     buf + pkt, sizeof(buf) - pkt);

	/* Enable the hardware-controlled tuning sequence. */
	pkt += fm_bop_modify(0x63, 0xfff8, 0x0001,
			     buf + pkt, sizeof(buf) - pkt);

	put_unaligned_le16(pkt - 4, buf + 2);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_TUNE_OPCODE, 5000);
	if (ret < 0)
		goto restore_desense;

	/*
	 * MT6628's FM event parser reports TUNE_DONE as a one-byte payload
	 * whose value must be 1.
	 */
	if (fm->cmd_data_len != 1 || fm->cmd_data[0] != 1)
	{
		ret = -EIO;
		goto restore_desense;
	}

	fm->freq = freq;
	return 0;

restore_desense:
	/*
	 * Desense state belongs to the actual tuned channel.  If the
	 * hardware tune fails, restore the state for the previous channel.
	 */
	mtk_fm_update_desense(fm, fm->freq);
	return ret;
}

static u16 mtk_fm_seek_spacing_code(u32 spacing)
{
	/*
	 * MT6628 supports 50/100/200 kHz seek spacing.  V4L2 passes
	 * spacing in Hz and permits the driver to select the nearest
	 * supported value.
	 */
	if (!spacing || spacing < 75000)
		return 0x1000;		/* 50 kHz */
	if (spacing < 150000)
		return 0x2000;		/* 100 kHz */

	return 0x4000;			/* 200 kHz */
}

static int mtk_fm_seek(struct mtk_fm *fm, bool seek_upward,
		       bool wrap_around, u32 rangelow, u32 rangehigh,
		       u32 spacing)
{
	u32 min_freq;
	u32 max_freq;
	u32 original_freq = fm->freq;
	u16 min_chan;
	u16 max_chan;
	u16 result;
	u16 spacing_code;
	u8 buf[128] = {};
	int pkt = 4;
	int ret;
	bool repositioned = false;

	/*
	 * With V4L2_TUNER_CAP_LOW the range is expressed in 62.5 Hz
	 * units.  MT6628 uses 10 kHz units internally.
	 */
	if (!rangelow)
		rangelow = 76 * 16000;
	if (!rangehigh)
		rangehigh = 108 * 16000;

	if (rangelow < 76 * 16000 ||
	    rangehigh > 108 * 16000 ||
	    rangelow > rangehigh)
		return -EINVAL;

	min_freq = DIV_ROUND_CLOSEST(rangelow, 160);
	max_freq = DIV_ROUND_CLOSEST(rangehigh, 160);

	if (min_freq < 7600 || max_freq > 10800 ||
	    min_freq > max_freq)
		return -EINVAL;

	/*
	 * VIDIOC_S_HW_FREQ_SEEK requires the current frequency to be
	 * inside the requested band before searching.
	 */
	if (fm->freq < min_freq || fm->freq > max_freq) {
		u32 freq = clamp_t(u32, fm->freq, min_freq, max_freq);

		ret = mtk_fm_tune(fm, freq);
		if (ret)
			return ret;

		repositioned = true;
	}

	min_chan = (min_freq - 6400) * 2 / 10;
	max_chan = (max_freq - 6400) * 2 / 10;
	spacing_code = mtk_fm_seek_spacing_code(spacing);

	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_SEEK_OPCODE;

	/*
	 * FM_MAIN_CFG1 (0x66):
	 *   bit 10      seek direction
	 *   bits 14:12  channel spacing
	 *   bit 11      wrap
	 *   bits 9:0    upper search bound
	 *
	 * FM_MAIN_CFG2 (0x67):
	 *   bits 9:0    lower search bound
	 */
	pkt += fm_bop_modify(0x66, 0xfbff,
			     seek_upward ? 0x0000 : 0x0400,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x66, 0x8fff, spacing_code,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x66, 0xf7ff,
			     wrap_around ? 0x0800 : 0x0000,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x66, 0xfc00, max_chan,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x67, 0xfc00, min_chan,
			     buf + pkt, sizeof(buf) - pkt);

	/* FM_MAIN_CTRL[2:0] = SEEK. */
	pkt += fm_bop_modify(0x63, 0xfff8, 0x0002,
			     buf + pkt, sizeof(buf) - pkt);

	if (pkt > sizeof(buf)) {
		ret = -E2BIG;
		goto restore;
	}

	put_unaligned_le16(pkt - 4, buf + 2);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_SEEK_OPCODE, 5000);
	if (ret)
		goto restore;

	if (fm->cmd_data_len < sizeof(result)) {
		ret = -EPROTO;
		goto restore;
	}

	/*
	 * The downstream FM event parser stores seek_result as a
	 * little-endian 16-bit frequency in 10 kHz units.
	 */
	result = get_unaligned_le16(fm->cmd_data);
	if (result < min_freq || result > max_freq) {
		ret = -ENODATA;
		goto restore;
	}

	fm->freq = result;
	return 0;

restore:
	/*
	 * V4L2 requires the original frequency to be restored after
	 * an unsuccessful hardware seek.
	 */
	if (fm->freq != original_freq || repositioned) {
		int restore_ret = mtk_fm_tune(fm, original_freq);

		if (restore_ret)
			dev_warn(fm->dev,
				 "failed to restore FM frequency %u: %d\n",
				 original_freq, restore_ret);
	}

	return ret;
}

/* ---- V4L2 ---- */

/*
 * V4L2_CID_AUDIO_MUTE is mapped onto the receiver mute bit in FM_MAIN_CTRL,
 * which is the same hardware mute that the downstream FM_IOCTL_MUTE path
 * drove through mt6628_Mute() (aquaris-5/.../mt6628/pub/mt6628_fm_lib.c:212-229).
 *
 * This is the tuner-side mute only.  This driver exposes no audio capture
 * path (there is no V4L2_CAP_AUDIO and no PCM device for the FM audio
 * chain), so this control silences the chip's own output rather than
 * attenuating a stream.
 */
/*
 * Report whether the chip is powered, i.e. whether a register write can be
 * issued at all.  All the hardware-backed controls below are cached when the
 * radio is closed and applied by mtk_fm_open() after power-up.
 */
static bool mtk_fm_is_powered(struct mtk_fm *fm)
{
	bool powered;

	mutex_lock(&fm->power_lock);
	powered = fm->users > 0;
	mutex_unlock(&fm->power_lock);

	return powered;
}

static int mtk_fm_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mtk_fm *fm =
		container_of(ctrl->handler, struct mtk_fm, ctrl_handler);
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		/*
		 * Record the request first.  The control can be set while the
		 * radio is closed, in which case the hardware is powered down
		 * and the register write cannot be issued; mtk_fm_open()
		 * then applies the cached value after power-up.  With no user
		 * of the radio open there is nothing listening anyway, so it
		 * is better to honour the request than to fail it.
		 */
		fm->mute = ctrl->val;

		if (!mtk_fm_is_powered(fm))
			return 0;

		return mtk_fm_set_mute(fm, ctrl->val);
	case V4L2_CID_AUDIO_VOLUME:
		fm->volume = ctrl->val;

		if (!mtk_fm_is_powered(fm))
			return 0;

		return mtk_fm_set_volume(fm, ctrl->val);
	case V4L2_CID_TUNE_DEEMPHASIS:
		fm->deemph_75us = ctrl->val == V4L2_DEEMPHASIS_75_uS;

		if (!mtk_fm_is_powered(fm))
			return 0;

		return mtk_fm_set_deemphasis(fm, fm->deemph_75us);
	case V4L2_CID_RDS_RECEPTION:
		fm->rds_on = ctrl->val;

		if (!mtk_fm_is_powered(fm))
			return 0;

		ret = mtk_fm_set_rds(fm, fm->rds_on);
		if (ret)
			return ret;

		/*
		 * Turning RDS on or off invalidates every decoded field, so the
		 * decoder state starts clean.  The vendor resets it in the same
		 * place (mt6628_RDS_OnOff() calls mt6628_RDS_Init_Data() for
		 * both directions, pub/mt6628_fm_rds.c:233-250).
		 */
		mtk_fm_rds_reset(fm);

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops mtk_fm_ctrl_ops = {
	.s_ctrl = mtk_fm_s_ctrl,
};

/*
 * Control descriptions for the two decoded RDS strings.
 *
 * A string control's maximum is the buffer size including the NUL
 * terminator, and the name is filled in by the control core when none is
 * given (see v4l2_ctrl_type_string(), drivers/media/v4l2-core/
 * v4l2-ctrls-core.c:2128).  The maxima match the buffer sizes in
 * struct mtk_fm_rds.
 */
static const struct v4l2_ctrl_config mtk_fm_rds_ps_cfg = {
	.ops = &mtk_fm_ctrl_ops,
	.id = V4L2_CID_RDS_RX_PS_NAME,
	.type = V4L2_CTRL_TYPE_STRING,
	.min = 0,
	.max = FM_RDS_PS_LEN,
	.step = 1,
};

static const struct v4l2_ctrl_config mtk_fm_rds_rt_cfg = {
	.ops = &mtk_fm_ctrl_ops,
	.id = V4L2_CID_RDS_RX_RADIO_TEXT,
	.type = V4L2_CTRL_TYPE_STRING,
	.min = 0,
	.max = FM_RDS_RT_LEN,
	.step = 1,
};

static int mtk_fm_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strscpy(cap->card, "MediaTek MT6628 FM Radio", sizeof(cap->card));
	cap->device_caps = vdev->device_caps;
	cap->capabilities = vdev->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int mtk_fm_g_tuner(struct file *file, void *priv,
			  struct v4l2_tuner *tuner)
{
	struct mtk_fm *fm = video_drvdata(file);
	u16 force_ms;
	u16 rssi_ind;
	int ret;

	if (tuner->index)
		return -EINVAL;

	ret = mtk_fm_read_reg(fm, FM_REG_FORCE_MS, &force_ms);
	if (ret)
		return ret;

	ret = mtk_fm_read_reg(fm, FM_REG_RSSI_IND, &rssi_ind);
	if (ret)
		return ret;

	strscpy(tuner->name, "FM", sizeof(tuner->name));
	tuner->type = V4L2_TUNER_RADIO;
	tuner->capability = V4L2_TUNER_CAP_LOW |
			    V4L2_TUNER_CAP_STEREO |
			    V4L2_TUNER_CAP_HWSEEK_BOUNDED |
			    V4L2_TUNER_CAP_HWSEEK_WRAP;
	/*
	 * With V4L2_TUNER_CAP_LOW, frequency units are 62.5 Hz.
	 * MT6628 internally uses 10 kHz units.
	 */
	tuner->rangelow = 76 * 16000;
	tuner->rangehigh = 108 * 16000;
	tuner->rxsubchans = (rssi_ind & FM_STEREO_IND) ?
			    V4L2_TUNER_SUB_STEREO :
			    V4L2_TUNER_SUB_MONO;
	tuner->audmode = (force_ms & FM_FORCE_MS) ?
			 V4L2_TUNER_MODE_MONO :
			 V4L2_TUNER_MODE_STEREO;
	tuner->signal = mtk_fm_rssi_to_signal(rssi_ind);
	/*
	 * The MT6628 has no AFC: downstream only uses AFC_ON to pick an
	 * alternative FM_MAIN_CTRL power-on value
	 * (aquaris-5/.../mt6628/inc/mt6628_fm.h:48-52) and never computes
	 * or reports an AFC value, so there is nothing to return here.
	 */
	tuner->afc = 0;

	return 0;
}

static int mtk_fm_s_tuner(struct file *file, void *priv,
			  const struct v4l2_tuner *tuner)
{
	struct mtk_fm *fm = video_drvdata(file);
	u16 val;
	int ret;

	if (tuner->index)
		return -EINVAL;

	switch (tuner->audmode) {
	case V4L2_TUNER_MODE_MONO:
	case V4L2_TUNER_MODE_STEREO:
		break;
	default:
		return -EINVAL;
	}

	/*
	 * MT6628's stereo/mono control register is accessed through the
	 * same clock/control window used by the downstream driver.
	 */
	ret = mtk_fm_write_reg(fm, FM_REG_CG_CTRL, 0x3007);
	if (ret)
		return ret;

	ret = mtk_fm_read_reg(fm, FM_REG_FORCE_MS, &val);
	if (ret)
		return ret;

	switch (tuner->audmode) {
	case V4L2_TUNER_MODE_MONO:
		val |= FM_FORCE_MS;
		break;
	case V4L2_TUNER_MODE_STEREO:
		val &= ~FM_FORCE_MS;
		break;
	}

	return mtk_fm_write_reg(fm, FM_REG_FORCE_MS, val);
}

static int mtk_fm_g_frequency(struct file *file, void *priv,
			      struct v4l2_frequency *frequency)
{
	struct mtk_fm *fm = video_drvdata(file);

	if (frequency->tuner)
		return -EINVAL;

	frequency->type = V4L2_TUNER_RADIO;
	frequency->frequency = fm->freq * 160;

	return 0;
}

static int mtk_fm_s_frequency(struct file *file, void *priv,
			      const struct v4l2_frequency *frequency)
{
	struct mtk_fm *fm = video_drvdata(file);
	u32 freq;

	if (frequency->tuner)
		return -EINVAL;

	if (frequency->frequency < 76 * 16000 ||
	    frequency->frequency > 108 * 16000)
		return -ERANGE;

	freq = DIV_ROUND_CLOSEST(frequency->frequency, 160);

	return mtk_fm_tune(fm, freq);
}

static int mtk_fm_enum_freq_bands(struct file *file, void *priv,
				  struct v4l2_frequency_band *band)
{
	if (band->tuner || band->index)
		return -EINVAL;

	if (band->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	band->capability = V4L2_TUNER_CAP_LOW |
			   V4L2_TUNER_CAP_STEREO |
			   V4L2_TUNER_CAP_HWSEEK_BOUNDED |
			   V4L2_TUNER_CAP_HWSEEK_WRAP;
	band->rangelow = 76 * 16000;
	band->rangehigh = 108 * 16000;
	band->modulation = V4L2_BAND_MODULATION_FM;

	return 0;
}

static int mtk_fm_s_hw_freq_seek(struct file *file, void *priv,
				 const struct v4l2_hw_freq_seek *seek)
{
	struct mtk_fm *fm = video_drvdata(file);

	if (seek->tuner || seek->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;

	return mtk_fm_seek(fm, !!seek->seek_upward,
			   !!seek->wrap_around, seek->rangelow,
			   seek->rangehigh, seek->spacing);
}

static const struct v4l2_ioctl_ops mtk_fm_ioctl_ops = {
	.vidioc_querycap	= mtk_fm_querycap,
	.vidioc_g_tuner		= mtk_fm_g_tuner,
	.vidioc_s_tuner		= mtk_fm_s_tuner,
	.vidioc_g_frequency	= mtk_fm_g_frequency,
	.vidioc_s_frequency	= mtk_fm_s_frequency,
	.vidioc_enum_freq_bands	= mtk_fm_enum_freq_bands,
	.vidioc_s_hw_freq_seek	= mtk_fm_s_hw_freq_seek,
	/*
	 * The control ioctls themselves are dispatched by the V4L2 core
	 * against fm->ctrl_handler; only the handler-side reporting and
	 * event plumbing has to be provided here.
	 */
	.vidioc_log_status	= v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations mtk_fm_fops = {
	.owner			= THIS_MODULE,
	.open			= mtk_fm_open,
	.release		= mtk_fm_release,
	.unlocked_ioctl		= video_ioctl2,
	.poll			= v4l2_ctrl_poll,
};

static int mtk_fm_probe(struct platform_device *pdev)
{
	struct mtk_fm *fm;
	struct mt6628_wmt *wmt;
	int ret;

	wmt = dev_get_drvdata(pdev->dev.parent);
	if (!wmt)
		return -EPROBE_DEFER;

	fm = devm_kzalloc(&pdev->dev, sizeof(*fm), GFP_KERNEL);
	if (!fm)
		return -ENOMEM;

	fm->dev = &pdev->dev;
	fm->wmt = wmt;
	init_completion(&fm->cmd_done);
	mutex_init(&fm->cmd_lock);
	mutex_init(&fm->power_lock);
	fm->waiting_opcode = 0xff;

	fm->freq = 8750;	/* 87.5 MHz in 10kHz units */

	ret = mt6628_stp_register_rx(wmt, MT6628_STP_TASK_FM,
				     mtk_fm_rx, fm);
	if (ret)
		return ret;

	ret = v4l2_device_register(&pdev->dev, &fm->v4l2_dev);
	if (ret)
		goto err_unregister_rx;

	v4l2_ctrl_handler_init(&fm->ctrl_handler, 1);
	fm->vdev.ctrl_handler = &fm->ctrl_handler;
	v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_fm_ctrl_ops,
			  V4L2_CID_AUDIO_MUTE, 0, 1, 1, 0);
	/*
	 * Volume is the chip's output gain, selected by index into the
	 * 16-entry table mtk_fm_set_volume() writes, so the maximum matches
	 * the table length.  As with mute this is a tuner-side control: there
	 * is no audio stream to attenuate.
	 */
	v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_fm_ctrl_ops,
			  V4L2_CID_AUDIO_VOLUME, 0, ARRAY_SIZE(mtk_fm_vol_tbl) - 1,
			  1, ARRAY_SIZE(mtk_fm_vol_tbl) - 1);
	/*
	 * De-emphasis is the standard 3-value menu (disabled / 50 us / 75 us).
	 * Only 50 and 75 us correspond to a hardware state, so "disabled"
	 * leaves FM_MAIN_CG2_CTRL[12] as the power-up sequence left it.
	 */
	v4l2_ctrl_new_std_menu(&fm->ctrl_handler, &mtk_fm_ctrl_ops,
			       V4L2_CID_TUNE_DEEMPHASIS,
			       V4L2_DEEMPHASIS_50_uS, 0, 0);
	/* RDS on/off, default off to match the power-up default. */
	v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_fm_ctrl_ops,
			  V4L2_CID_RDS_RECEPTION, 0, 1, 1, 0);

	/*
	 * RDS data controls.  Their values come from the demodulator, never
	 * from userspace, so they are created read-only.
	 */
	fm->rds_ps = v4l2_ctrl_new_custom(&fm->ctrl_handler,
					  &mtk_fm_rds_ps_cfg, fm);
	fm->rds_rt = v4l2_ctrl_new_custom(&fm->ctrl_handler,
					  &mtk_fm_rds_rt_cfg, fm);
	fm->rds_pty = v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_fm_ctrl_ops,
					V4L2_CID_RDS_RX_PTY, 0, 31, 1, 0);
	fm->rds_ta = v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_fm_ctrl_ops,
				       V4L2_CID_RDS_RX_TRAFFIC_ANNOUNCEMENT,
				       0, 1, 1, 0);
	fm->rds_tp = v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_fm_ctrl_ops,
				       V4L2_CID_RDS_RX_TRAFFIC_PROGRAM,
				       0, 1, 1, 0);
	fm->rds_ms = v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_fm_ctrl_ops,
				       V4L2_CID_RDS_RX_MUSIC_SPEECH,
				       0, 1, 1, 0);
	if (fm->ctrl_handler.error) {
		ret = fm->ctrl_handler.error;
		v4l2_err(&fm->v4l2_dev, "failed to init controls: %d\n", ret);
		goto err_free_ctrls;
	}
	/* The decoded RDS fields are written by the driver, not by userspace. */
	fm->rds_ps->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	fm->rds_rt->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	fm->rds_pty->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	fm->rds_ta->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	fm->rds_tp->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	fm->rds_ms->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	/*
	 * Do not call v4l2_ctrl_handler_setup() here: it would run the mute
	 * control's s_ctrl against hardware that is still powered off.
	 * mtk_fm_open() applies the current value after powering up instead.
	 */

	fm->vdev.v4l2_dev = &fm->v4l2_dev;
	fm->vdev.fops = &mtk_fm_fops;
	fm->vdev.ioctl_ops = &mtk_fm_ioctl_ops;
	fm->vdev.device_caps = V4L2_CAP_RADIO |
			       V4L2_CAP_TUNER |
			       V4L2_CAP_HW_FREQ_SEEK;
	fm->vdev.release = video_device_release_empty;
	video_set_drvdata(&fm->vdev, fm);

	ret = video_register_device(&fm->vdev, VFL_TYPE_RADIO, -1);
	if (ret) {
		v4l2_err(&fm->v4l2_dev, "failed to register radio: %d\n", ret);
		goto err_free_ctrls;
	}

	platform_set_drvdata(pdev, fm);

	return 0;

err_free_ctrls:
	v4l2_ctrl_handler_free(&fm->ctrl_handler);
	fm->vdev.ctrl_handler = NULL;
	v4l2_device_unregister(&fm->v4l2_dev);
err_unregister_rx:
	mt6628_stp_unregister_rx(wmt, MT6628_STP_TASK_FM,
				 mtk_fm_rx, fm);
	return ret;
}

static void mtk_fm_remove(struct platform_device *pdev)
{
	struct mtk_fm *fm = platform_get_drvdata(pdev);

	if (!fm)
		return;

	video_unregister_device(&fm->vdev);
	mtk_fm_power_put(fm);
	v4l2_ctrl_handler_free(&fm->ctrl_handler);
	v4l2_device_unregister(&fm->v4l2_dev);
	mt6628_stp_unregister_rx(fm->wmt, MT6628_STP_TASK_FM,
				 mtk_fm_rx, fm);
}

static struct platform_driver mtk_fm_driver = {
	.probe = mtk_fm_probe,
	.remove = mtk_fm_remove,
	.driver = {
		.name = "mt6628-fm",
	},
};
module_platform_driver(mtk_fm_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 FM radio driver");
MODULE_LICENSE("GPL");
