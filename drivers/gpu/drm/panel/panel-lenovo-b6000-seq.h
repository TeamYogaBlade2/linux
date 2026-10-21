/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Init sequences for the Lenovo B6000 panels.
 *
 * Decoded from kernel_blade8_row_3gdata-jb.elf; see panel-lenovo-b6000.c for
 * the word-packing derivation and the cross-checks that validate it.
 */
#ifndef _PANEL_LENOVO_B6000_SEQ_H
#define _PANEL_LENOVO_B6000_SEQ_H

#include <linux/types.h>

/*
 * One entry of a panel init sequence.  A delay entry has delay_ms set and no
 * command; otherwise cmd/len/data describe a DCS write.
 */
struct b6000_init_cmd {
	u8 cmd;
	u8 len;
	const char *data;
	u16 delay_ms;
};

static const struct b6000_init_cmd b6000_cmi_init[] = {
	{ .cmd = 0xff, .len = 4, .data = "\xaa\x55\xa5\x80" },
	{ .cmd = 0x6f, .len = 1, .data = "\x07" },
	{ .cmd = 0xf7, .len = 1, .data = "\x50" },
	{ .cmd = 0x6f, .len = 2, .data = "\x11\x00" },
	{ .cmd = 0xf7, .len = 2, .data = "\x20\x00" },
	{ .cmd = 0x6f, .len = 1, .data = "\x06" },
	{ .cmd = 0xf7, .len = 1, .data = "\xa0" },
	{ .cmd = 0x6f, .len = 1, .data = "\x19" },
	{ .cmd = 0xf7, .len = 1, .data = "\x12" },
	{ .cmd = 0x6f, .len = 1, .data = "\x08" },
	{ .cmd = 0xfa, .len = 1, .data = "\x40" },
	{ .cmd = 0x6f, .len = 1, .data = "\x11" },
	{ .cmd = 0xf3, .len = 1, .data = "\x01" },
	{ .cmd = 0xf0, .len = 5, .data = "\x55\xaa\x52\x08\x00" },
	{ .cmd = 0xc8, .len = 1, .data = "\x00" },
	{ .cmd = 0xb1, .len = 2, .data = "\x68\x01" },
	{ .cmd = 0xb6, .len = 1, .data = "\x08" },
	{ .cmd = 0x6f, .len = 1, .data = "\x02" },
	{ .cmd = 0xb8, .len = 1, .data = "\x0c" },
	{ .cmd = 0xbb, .len = 2, .data = "\x74\x44" },
	{ .cmd = 0xbc, .len = 2, .data = "\x00\x00" },
	{ .cmd = 0xbd, .len = 5, .data = "\x02\xb0\x0c\x0a\x00" },
	{ .cmd = 0xf0, .len = 5, .data = "\x55\xaa\x52\x08\x01" },
	{ .cmd = 0xb0, .len = 2, .data = "\x05\x05" },
	{ .cmd = 0xb1, .len = 2, .data = "\x05\x05" },
	{ .cmd = 0xbc, .len = 2, .data = "\x90\x01" },
	{ .cmd = 0xbd, .len = 2, .data = "\x90\x01" },
	{ .cmd = 0xca, .len = 1, .data = "\x00" },
	{ .cmd = 0xc0, .len = 1, .data = "\x04" },
	{ .cmd = 0xbe, .len = 1, .data = "\x29" },
	{ .cmd = 0xb3, .len = 2, .data = "\x37\x37" },
	{ .cmd = 0xb4, .len = 2, .data = "\x19\x19" },
	{ .cmd = 0xb9, .len = 2, .data = "\x46\x46" },
	{ .cmd = 0xba, .len = 2, .data = "\x24\x24" },
	{ .cmd = 0xf0, .len = 5, .data = "\x55\xaa\x52\x08\x02" },
	{ .cmd = 0xee, .len = 1, .data = "\x01" },
	{ .cmd = 0xef, .len = 4, .data = "\x09\x06\x15\x18" },
	{ .cmd = 0x24, .len = 6, .data = "\x00\x55\x00\xb0\x00\x00" },
	{ .cmd = 0x6f, .len = 1, .data = "\x06" },
	{ .cmd = 0x94, .len = 6, .data = "\x00\xc0\x00\xb0\x00\x77" },
	{ .cmd = 0x6f, .len = 1, .data = "\x0c" },
	{ .cmd = 0xb0, .len = 4, .data = "\x00\xe3\x01\x1a" },
	{ .cmd = 0x88, .len = 6, .data = "\x01\xbc\x00\xb1\x01\x46" },
	{ .cmd = 0x6f, .len = 1, .data = "\x06" },
	{ .cmd = 0x4b, .len = 6, .data = "\x02\x4d\x00\xb1\x02\x0b" },
	{ .cmd = 0x6f, .len = 1, .data = "\x0c" },
	{ .cmd = 0xb1, .len = 4, .data = "\x02\x88\x02\xc9" },
	{ .cmd = 0x29, .len = 6, .data = "\x03\x4e\x00\xb2\x02\xf3" },
	{ .cmd = 0x6f, .len = 1, .data = "\x06" },
	{ .cmd = 0x9b, .len = 6, .data = "\x03\xbe\x00\xb2\x03\x7d" },
	{ .cmd = 0x6f, .len = 1, .data = "\x0c" },
	{ .cmd = 0xb2, .len = 4, .data = "\x03\xd3\x03\xe9" },
	{ .cmd = 0xb3, .len = 4, .data = "\x03\xfb\x03\xff" },
	{ .cmd = 0xf0, .len = 5, .data = "\x55\xaa\x52\x08\x06" },
	{ .cmd = 0xb0, .len = 2, .data = "\x00\x10" },
	{ .cmd = 0xb1, .len = 2, .data = "\x12\x14" },
	{ .cmd = 0xb2, .len = 2, .data = "\x16\x18" },
	{ .cmd = 0xb3, .len = 2, .data = "\x1a\x29" },
	{ .cmd = 0xb4, .len = 2, .data = "\x2a\x08" },
	{ .cmd = 0xb5, .len = 2, .data = "\x31\x31" },
	{ .cmd = 0xb6, .len = 2, .data = "\x31\x31" },
	{ .cmd = 0xb7, .len = 2, .data = "\x31\x31" },
	{ .cmd = 0xb8, .len = 2, .data = "\x31\x0a" },
	{ .cmd = 0xb9, .len = 2, .data = "\x31\x31" },
	{ .cmd = 0xba, .len = 2, .data = "\x31\x31" },
};
static const struct b6000_init_cmd b6000_cpt_init[] = {
	{ .delay_ms = 10 },
	{ .delay_ms = 10 },
	{ .delay_ms = 120 },
	{ .cmd = 0xf0, .len = 2, .data = "\x5a\x5a" },
	{ .cmd = 0xf1, .len = 2, .data = "\x5a\x5a" },
	{ .cmd = 0xfc, .len = 2, .data = "\xa5\xa5" },
	{ .cmd = 0xd0, .len = 2, .data = "\x00\x10" },
	{ .cmd = 0xb1, .len = 1, .data = "\x10" },
	{ .cmd = 0xb2, .len = 4, .data = "\x14\x22\x2f\x04" },
	{ .cmd = 0xb5, .len = 1, .data = "\x01" },
	{ .cmd = 0xee, .len = 8, .data = "\x00\x00\x1f\x00\x00\x00\x1f\x00" },
	{ .cmd = 0xef, .len = 13, .data = "\x56\x34\x43\x65\x90\x80\x24\x80\x00\x91\x11\x11\x11" },
	{ .cmd = 0xf7, .len = 32, .data = "\x04\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x16\x17\x10\x01\x01\x01\x01\x04\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x16\x17\x10\x01\x01\x01\x01" },
	{ .cmd = 0xf2, .len = 5, .data = "\x02\x08\x08\x40\x10" },
	{ .cmd = 0xf6, .len = 6, .data = "\x60\x25\x26\x00\x00\x00" },
	{ .cmd = 0xfa, .len = 17, .data = "\x04\x35\x07\x0b\x12\x0b\x10\x16\x1a\x24\x2c\x33\x3b\x3b\x33\x34\x33" },
	{ .cmd = 0xfb, .len = 17, .data = "\x04\x35\x07\x0b\x12\x0b\x10\x16\x1a\x24\x2c\x33\x3b\x3b\x33\x34\x33" },
	{ .cmd = 0xf3, .len = 10, .data = "\x01\xc4\xe0\x62\xd4\x83\x37\x3c\x24\x00" },
	{ .cmd = 0xf4, .len = 35, .data = "\x00\x02\x03\x26\x03\x02\x09\x00\x07\x16\x16\x03\x00\x08\x08\x03\x19\x1c\x12\x1c\x1d\x1e\x1a\x09\x01\x04\x02\x61\x74\x75\x72\x83\x80\x80\xf0" },
	{ .cmd = 0xb0, .len = 1, .data = "\x01" },
	{ .cmd = 0xf5, .len = 19, .data = "\x2f\x2f\x5f\xab\x98\x52\x0f\x33\x43\x04\x59\x54\x52\x05\x40\x40\x5d\x59\x40" },
	{ .cmd = 0xbc, .len = 3, .data = "\x01\x4e\x08" },
	{ .cmd = 0xe1, .len = 5, .data = "\x03\x10\x1c\xa0\x10" },
	{ .cmd = 0xfd, .len = 5, .data = "\x16\x10\x11\x20\x09" },
	{ .delay_ms = 120 },
};
#endif /* _PANEL_LENOVO_B6000_SEQ_H */
