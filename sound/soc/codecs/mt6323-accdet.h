/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2026 Roman Vichar <rva333@protonmail.com>
 */

#ifndef _MT6323_ACCDET_H_
#define _MT6323_ACCDET_H_

#include <sound/soc-component.h>

int mt6323_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack);
#endif
