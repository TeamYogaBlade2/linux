# PR #75 readiness assessment

**Bottom line: nothing in this tree has been validated on hardware.** Every claim
below is from source review against `./aquaris-5` (downstream) and the MT6589
datasheet. "Ready" here means *reviewed and self-consistent*, not *booted*.

The datasheet was treated as one opinion among several, not as authority. Where
downstream code and the datasheet agreed, I said so and used it. Where the
datasheet was the only source, I said that too, and where it was contradicted by
downstream I followed downstream. Two findings below rest on the datasheet alone.

## Tier 1 — highest confidence, downstream-backed

| Driver | Basis | Notes |
|---|---|---|
| **MT6320 ASoC codec** | downstream register headers + `upmu_hw.h` bitfields | AFE window 0x2000→0x4000 confirmed by two independent downstream headers *and* the absence of any 0x2000 mapping in the kernel's own register file. Speaker trim and `SPK_CON11` confirmed against `upmu_hw.h`. |
| **MT6320 ACCDET** | downstream `accdet.c` + Blade `accdet_custom.c` | Tuning constants matched field-for-field against `cust_headset_settings`. Key thresholds 90/240/500 mV, 60 ms period, TOP_CKPDN bit 14 all confirmed against downstream. |
| **MT6589 AFE register map** | downstream `mt_soc_afe_control.c` / `mt_soc_pcm_afe.c` | The transcription is genuinely good: all register offsets, `SetDLSrc2()` at all 9 rates, connection bits and IRQ/DAC bitfields verified byte-for-byte. The bugs I fixed were in *sequencing and clock ownership*, not in the register data. |
| **MT6628 WLAN (STA)** | downstream `drv_wlan/mt6628/wlan` | Command/event IDs, TX_DONE structure, CH_PRIVILEGE, CH_REQ_TYPE enum and RCPI_TO_dBm all verified against downstream source. |

These four are the parts I would trust most, because each claim is backed by
downstream code that demonstrably runs on this silicon.

## Tier 2 — good confidence, mixed basis

| Driver | Basis | Caveat |
|---|---|---|
| **MT6589 DRM display** | downstream + datasheet | Most register work confirmed against downstream. Two fixes rest on the datasheet alone: `OVL_CON[23:16]` is `HORI_BLOCK_NUM` (so the Y2R matrix-enable bit was corrupting it), and `DSI_PHY_TIMCON2[15:8]` is reserved (downstream's struct independently names it `RSV8`, which is corroboration). Clock-gating fix confirmed by downstream enabling all three clocks. |
| **MT6628 combo: STP / BT / FM / GNSS** | downstream | Large implementations present. **I did not review these at all** — they predate this session. Treat as unreviewed. |

## Audio scope: DL1 playback + VUL capture

Both DAIs are now bidirectional: `mt6589-afe-vul` capture was added alongside
`mt6589-afe-dl1` playback, the MT6320 codec has a capture stream on its AIF1, and
the machine driver has a VUL Capture DAI link.  `arecord` should work.

The VUL path follows downstream `mt_soc_pcm_capture.c`, and every register was
checked rather than assumed:

- DMA ring at AFE_VUL_BASE/END/CUR (0x0080/0x0088/0x008c), confirmed in the
  datasheet.
- **Sample-rate codes — two different encodings exist and only one was
  implemented.**  `SetDLSrc2()` has its own dense 0..8 table used by
  `AFE_ADDA_DL_SRC2_CON0`.  Everything else goes through
  `SampleRateTransform()`, which returns the *sparse* `Soc_Aud_I2S_SAMPLERATE_*`
  enum where 16k is 4 not 3 and 44.1k is 9 not 7.  This affects
  `AFE_I2S_CON1_RATE`, `AFE_DAC_CON1_DL1_RATE`, `AFE_DAC_CON1_VUL_RATE`
  and both IRQ rate fields - so it affected DL1 **playback** too, not just
  capture.  Every rate from 16k upwards was programmed with the wrong code.
  Both tables are now present and each field uses the right one.
- **IRQ2 counts into `AFE_IRQ_MCU_CNT2`** (0x03b0), not CNT1; starting capture
  would have overwritten the DL1 period counter.
- **The ADC to VUL paths are in `AFE_CONN3`** (0x02c) bits 0 and 3, not
  AFE_CONN2.  The data sheet names those bits `I03_O09_S`/`I04_O10_S` while
  the downstream tables label the same register and bits `I03->O10`/`I04->O09`
  - the two sources are offset by one in output numbering, so register and bit
  were taken as the common ground.
- VUL memif enable at `AFE_DAC_CON0` bit 3 — the datasheet shows one enable per
  memif (DL1 bit 1, DL2 bit 2, VUL bit 3, AWB bit 4) and downstream uses
  `1 << (block + 1)` with `MEM_DL1 == 0`.  An earlier version of this commit
  wrongly reused the DL1 bit, which would have started playback instead.
- VUL sample rate in `AFE_DAC_CON1[19:16]` and channel select at bit 27, from
  downstream `SetSampleRate`/`SetChannels` for `MEM_VUL` (masks 0x000f0000 and
  1<<27) — my values match exactly.
- IRQ2 enable at bit 1, rate at [11:8], counter in IRQ_MCU_CNT1, all confirmed
  in the datasheet.
- I2S ADC to VUL routing `AFE_CONN2` bit 29 (I04→O09) and bit 0 (I03→O10),
  extracted from the downstream `mConnectionReg`/`mConnectionbits` tables.
- Uplink SRC enable at `AFE_ADDA_UL_SRC_CON0` bit 0 and internal-ADC select at
  `AFE_ADDA_TOP_CON0` bit 0, per `SetI2SAdcEnable()`/`SetI2SAdcIn()`.

### Remaining downstream features, and why each is absent

These were investigated rather than simply skipped.  In each case the
downstream path exists for a consumer that either does not exist on this board
or belongs to the voice subsystem:

- **AWB, second I2S, hardware digital gain** — these three form one chain whose
  only purpose downstream is carrying FM radio audio: `mt_soc_fm_i2s2.c` is the
  sole caller of `SetHwDigitalGain()`, and the AWB platform's only machine-driver
  link is `FM_I2S2_IN`.  In this tree the FM driver is
  `drivers/net/wireless/mediatek/mt6628/mtk-fm.c` and it registers only
  `V4L2_CAP_RADIO | V4L2_CAP_TUNER | V4L2_CAP_HW_FEQ_SEEK` — no
  `V4L2_CAP_AUDIO` and no PCM stream.  It is also entirely STP-based with no I2S
  wiring at all.  So there is no producer: adding the chain would yield capture
  endpoints that can never be opened and a second I2S input with no codec node.
- **DL2** — dead upstream too.  The only reference to `MEM_DL2` anywhere in the
  downstream kernel tree is `mt_soc_pcm_afe.c:573`, which formats DL1's 32-bit
  data; it is never enabled as a path.
- **DAI / MOD_DAI and sidetone** — outputs consumed by the voice path
  (`mt_soc_voice.c`, PCM2_VOICE, modem PCM).  These are Android speech-subsystem
  features, not general playback or capture, and the modem-facing handshake has
  no meaning without that stack.
- **Voice/modem PCM** — same reasoning; out of scope for a Wi-Fi tablet.

### Mixer controls

Only a headphone volume control exists, and that is the complete set the kernel
side can offer: the downstream kernel sound driver has no volume API at all
(`SetVolume`/`SetAnaVolume`/`AUDIO_VOLUME` appear nowhere under
`kernel/sound/soc/mediatek`), the ZCD gain registers are only read for debug
dumps and never programmed, and volume is handled in the Android audio HAL.
The one candidate register field I had considered for a speaker gain,
`AUD_IV_CFG0[4:2]`, turned out to be the speaker mux rather than a gain.

Present and reasonable for the scope: DL1 playback, 8–48 kHz S16_LE, headphone and
speaker analog paths with full power sequencing, de-pop, NCP and EFUSE trimming,
plus headset detection and three button keys wired to the ALSA jack.

So the honest description is *a minimal ASoC port that makes DL1 playback work*,
not a complete MT6589 audio driver.  The natural next increment is VUL capture and
the MT6320 ADC path; everything else is larger again.

## Tier 3 — explicitly not ready

- **MT6628 roaming** — deliberately not ported. The firmware event carries only
  4 bytes (`{u2Event, u2Data}`) with no BSSID; the target AP is chosen inside a
  583-line FSM coupled to a ~1100-line AIS FSM that does not exist here.
- **`get_survey`** — impossible on this firmware; no survey/noise command exists.
- **MT6628 AP / P2P / monitor, VHT, DFS+regulatory, PMF, SAE, 802.1X** — not
  implemented, each large or firmware-limited.

## Bugs found by reviewing this session's own new code

Worth stating plainly: every feature added this session had at least one real
defect, and two were completely non-functional on hardware while building cleanly
and carrying convincing commit messages. Seven such bugs were found, all in
interface interactions rather than in any single function's logic:

1. Reconnect after firmware reset never worked — recovery's teardown freed the
   saved parameters before the retry could use them.
2. Sleeping in hard IRQ — the SDIO IRQ handler drove an 8-second ownership poll.
3. The ownership FSM reclaimed Driver Own only via the 32-bit register helpers;
   all six bulk transfers bypassed them, so a queued data frame went to a chip the
   driver no longer owned and its TX resource was never released.
4. A ROC window was never released on expiry, blocking later requests.
5. A partial retry copy could be used to re-associate.
6. `get_station` reported raw RCPI as dBm (128 → 128 dBm instead of −46 dBm).
7. Key-mask bookkeeping raced disconnect teardown, orphaning a key in firmware.

## Hardware-only unknowns

These cannot be settled by reading source:

- **DSI link rate** — the mode needs 419.022 Mbps; the PLL table's nearest entry is
  416 Mbps (0.721% deficit). I verified the PHY already selects the nearest entry
  and that *every* DPHY timing field evaluates identically at both rates, so no
  register is miscomputed. Whether a 0.72% slow link tears on this panel is a
  panel property.
- **Panel DCS init** — `boe,hx8896-a01` has no `.init_sequence`. Its delays
  (120/100/320 ms) are specific rather than defaults, suggesting they were
  measured, but aquaris-5 has no MT6589 1280×800 DSI panel to check against.
- **`CONT_DET`** — left at reset rather than guessed; downstream sources it from LCM
  parameters with no ground truth available here.
- **RDMA interrupt behaviour** — `mtk_rdma_stop()` does not mask `INT_ENABLE`/ack
  `INT_STATUS` as downstream does, so a level-triggered SPI could re-fire. Left
  alone: shared across all MediaTek SoCs and this is upstream's pattern.
- **Display power domains** — OVL/RDMA declare a domain but take no reference. Inert
  here (RDMA0's own `pm_runtime_get_sync()` powers it on), and upstream behaviour.

## MT6628 WLAN gap analysis

Full gap analysis against the downstream driver:
`drivers/net/wireless/mediatek/mt6628/Downstream-Gaps.md`.  It separates the
features that are missing because the *firmware* cannot support them
(`get_survey`, upstream-style roaming, the FM audio chain, DL2) from those that
are out of scope (voice/modem PCM) and those that are simply not implemented
(AP/P2P/monitor, VHT, DFS/regulatory, PMF, SAE, 802.1X).

## MT6589 clock driver address audit

Every register offset in all ten `clk-mt6589-*.c` drivers was resolved against
the physical address (DT node base + offset) and checked in the data sheet:

- **topckgen** (`base 0x10000100`): `CLK_CFG_0..8` at 0x40..0x64 and
  `CLK_PDN_SET/CLR/STA` at 0x70/0x74/0x78 all resolve to the matching data sheet
  registers.  The `+0x100` slide is already in place, and `TOPRGU_BASE` is
  `0x10000000`, so there is no collision with the watchdog at `0x10000000`.
- **infracfg** (`base 0x10001000`): `TOP_CKMUXSEL` at 0x00 and `TOP_CKDIV1` at
  0x08 are *infracfg* registers despite the `TOP_` prefix, and resolve to
  `0x10001000` / `0x10001008`, which is where the data sheet lists them.
  `TOP_DCMCTL` is likewise an infracfg register at 0xA0.  Nothing in
  topckgen claims them.
- **pericfg** (`base 0x10003000`): all six PDN set/clr/sta registers resolve to
  `PERI_GLOBALCON_PDN0/1` at 0x08..0x1C.
- **apmixedsys** (`base 0x10209000`): every PLL register resolves; `VOID_REG` at
  offset 0 is the intentional "no post-divider" placeholder for the LC PLLs.
- **disp** (`0x14000000`), **img** (`0x15000000`), **vdec** (`0x16000000`),
  **venc** (`0x17000000`), **mfg** (`0x10206000`), **aud** (`0x12070000`): all
  offsets match the data sheet, or - for the image/video domains the data sheet
  does not document - the downstream `mt_clkmgr.h` register definitions.

Two naming traps worth remembering, since both look wrong at first glance:
`TOP_CKMUXSEL`/`TOP_CKDIV1`/`TOP_DCMCTL` belong to infracfg, not topckgen, and
the data sheet's summary table splits `PERI_GLOBALCON_PDN*` across lines so a
naive address grep misses them.

Gate bit positions, mux parents and PLL fields were verified separately and are
correct; see the decisions log.

## Practical first-boot checklist

In rough order of expected information value:

1. **Display** — exercises the clock-gating and `OVL_CON` fixes, the two changes most
   likely to produce a visible difference. Watch for a blank or corrupted panel.
2. **Wi-Fi association** — exercises the ownership FSM and the bulk-path fix.
3. **Audio playback, then headset jack** — exercises the AFE window fix (the single
   most consequential audio change).

For each, check `dmesg` for the warnings the drivers already emit on the failure
paths I fixed — those are the intended tripwires.
---

## Boot bring-up findings (from real hardware)

Recorded because each was invisible to source review and each presented only
as a missing log line.

- **MT6589 had no MMC compatible.** The nodes declare `mediatek,mt6589-mmc`,
  which matched nothing in `mtk-sd.c`, so the controller never probed.  Symptom:
  *no MMC or SDIO output at all*, and the MT6628 combo functions never
  enumerated.  Added a compatible derived from mt8173 - the data sheet gives
  `MSDC_CFG` `CARD_CK_DIV` as bits [15:8], so the divider is 8 bits wide, not the
  12 of the mt8183 class.  `needs_top_base` stays false because MT6589 drives MSDC
  from TOPCKGEN and the nodes declare a single reg window.
- **The MT6628 WLAN SDIO function node was missing.** The combo chip exposes WLAN
  as SDIO function 1 and STP (BT/FM/GNSS) as function 2; only function 2 existed,
  so `mt6628_wlan_sdio_probe()` was never called.
- **Frequency hopping must not be used on MT6589.**  Downstream never brings the
  block up: `mt_freqhopping_init()` is commented out and `mt_fh_hal_init()` is only
  stored in a table nothing calls.  ARMPLL DVFS writes `ARMPLL_CON1` directly.
  Registering the PLLs with the FHCTL helper sends every rate change through
  `hopping()`, which polls a monitor register that never updates and leaves the
  PLL unprogrammed - the ARM clock stops and the boot hangs.
- **AP-side register window.**  Downstream addresses the non-CPU domains as
  `0xFxxx_xxxx`; the AP aliases them as `0x1xxx_xxxx` with the low 28 bits
  preserved (FHCTL `0xF1005000` = AP `0x11005000`).  Changing a node to the
  downstream value breaks it.
- **The simple-framebuffer console is not the display path.**  The board drives
  its panel over DSI through mediatek-drm; the framebuffer node was only a
  fallback and has been removed.

