# MT6589 Lenovo Yoga Tablet (B8000-F) — port status

Branch `dev/v7.1/mt6589-drm-ox-alpha` → origin (PR #75). 319 commits ahead of
`origin/blade/v7.1`, of which 19 are this work. All Signed-off-by-free.
Verified: `make ARCH=arm LLVM=1 lenovo-blade_defconfig && make -j$(nproc)` clean,
no warnings, from a fresh defconfig.

Target DTS: `arch/arm/boot/dts/mediatek/mt6589-lenovo-b8000-f.dts`
  → `mt6589-lenovo-b8000.dtsi` → `mt6589-lenovo-blade.dtsi`
  The sound card (`mediatek,mt6589-mt6320-sound`) and `&mt6320_accdet` live in
  mt6589-lenovo-blade.dtsi, so all the audio work applies to this board.

## Audio — complete

Later confirmed on hardware: the card registers (`ALSA device list: #0:
mt6589-mt6320`) after four structural breaks that each masked the next, so a
dozen individually-correct register fixes produced silence:

- MT6797 ADDA/NEWIF registers had been adopted into the MT6589 AFE map. Seven
  offsets do not exist on this part, and they were written on the live path, not
  just at probe.
- Neither dai link had a codec bound to it, so `soc_find_component()` returned
  NULL and the card never registered at all — which made every codec-side fix
  inert.
- The AFE component had no DAPM graph, so the codec's routes had no endpoints.
- The stream widgets had to be named after the DAI `stream_name`s
  (`"DL1 Playback"`, `"VUL Capture"`); a DAI is auto-connected through the
  widget carrying that name, so separate `"AIF1 ..."` endpoints went undriven.

A system hang on the first period interrupt was the level-triggered AFE IRQ
announcing the period before acking it, so the handler re-entered
indefinitely. And `AFUNC_AUD_CON2` bit 7 is a *mute*, not an enable: the stock
driver asserts it while configuring a path and clears it afterwards, where here
it was set on power-up and only cleared on power-down, leaving the output muted
for as long as it was playing.

Fixed, each verified against the downstream source rather than assumed:

- AFE register window was `0x2000`; downstream `AudioAnalogReg.h`/`AudDrv_Ana.h`
  both say `AFE_PMICDIG_AUDIO_BASE` is `0x4000`. Every digital AFE write was
  landing at the wrong address, and the MT6320 register map has nothing at
  `0x2000` at all.
- `NEWIF_CFG0..3` and `UP8X_FIFO_CFG0` were programmed in the PMIC codec but do
  not exist on this part (no NEWIF symbol anywhere in the aquaris-5 tree). The
  serial link is the SoC AFE driver's job; removed from the codec.
- `SPK_CON11` written as a plain enable word; it is a one-hot software-override
  select (`SPK_EN_MODE` bit 0, `SPK_EN_L_SW` bit 9, `SPK_OUTSTG_EN_L_SW` bit 11
  per upmu_hw.h).
- E2 speaker auto-trim read `0x013a`/`0x014e` = `TEST_CON0`/`TEST_OUT_L`, a test
  data bus. The measured offset is latched in `SPK_CON1` (bit 14 mode, bits 12:8
  offset) — exactly the layout the E1 branch above already used.
- `AUD_NCP0` set `0xe000`, whose bits 14:13 the power-off path then cleared, so
  the regulator never returned to a documented idle value. Now the per-revision
  value the BSP `power_init()` uses (0x8000 E2, 0x9000 E1).
- AFE probe wrote `AUDIO_TOP_CON0` in full, stomping the CCF clock gates for AFE
  (bit 2) and I2S (bit 6) that `devm_clk_get_enabled()` had just set through the
  clk provider. Also: `NEWIF_CFG1` was a fabricated constant, an inherited
  `AFE_MEMIF_MAXLEN` write removed, and DL1 now starts before its period IRQ.
- ACCDET: micbias/AUXADC switch left driving an empty jack on unplug, hook-switch
  debounce never shortened and PWM duty never re-asserted in MIC_BIAS, and the key
  scanner was not cancelled on unplug.
- ACCDET key voltage now read via the IIO core rather than duplicated constants.

Two questions settled rather than "fixed": AFE `IRQ_MCU_STATUS` DL1 is bit 0
(datasheet), so the existing code was already right and downstream's 1-based enum
is a legacy artifact; and the `0x01c2`/`0x01c4` HP-trim reads really are correct
(efuse data-out words), only the local naming was misleading.

## DRM — defects found and fixed

`f84c1dc1ec1d` inserted `DDP_COMPONENT_TDSHP` into the MT6589 main path. No such
route exists: `mt6589-dispsys.h` documents `OVL → COLOR → BLS → RDMA0 → DSI0` and
its routing table has `COLOR0 → BLS` with no TDSHP entry. `mtk_mmsys_ddp_connect()`
returns `void` and silently no-ops on an unmatched pair, so that replaced the only
code programming `COLOR_MOUT_EN`/`BLS_SEL_IN` with two no-ops — CRTC with no
output. Reverted just that hunk; the mutex mapping and `->stop()` fix were correct
and were kept.

## DRM — diagnosed from hardware, not inference

Five rounds of static analysis could not separate the display failures, because
"engine never started", "RDMA stalled fetching" and "SOF never triggered" all
produced byte-identical logs. Adding an `OVL_STA` readback to the IRQ handler
broke the tie in one boot:

	OVL: underflow intsta=0x35 sta=0x1d (run=1 rdma0_idle=0)

`OVL_RUN=1` said the overlay had started and was waiting; `RDMA0_IDLE=0` said
RDMA0 was mid-transfer and never completing. Two further fixes followed from
hardware evidence, each fixing a case the logs could not distinguish:

- **RDMA0 was in memory mode.** `MODE_SEL=1` points RDMA0 at its own
  `MEM_MODE` ring, but nothing attaches a plane to RDMA0 on this configuration:
  `mtk_crtc_num_comp_planes()` creates planes only for components 0 and 1, and
  MT6589's COLOR declares none while OVL claims all four. RDMA0 fetched from an
  address no plane supplied. The stock driver uses `RDMA_MODE_DIRECT_LINK` for
  this path. After this the underflow spam disappeared entirely.
- **DSI drove zero lanes.** `mtk_dsi_rxtx_control()` writes `DSI_TXRX_CTRL`,
  which carries `LANE_NUM`, and it was only ever called from `mtk_dsi_stop()` —
  never from `mtk_dsi_poweron()`. `LANE_NUM` sat at its reset value of 0. That
  is exactly the "no spam, no picture" signature: the pipeline ran correctly and
  nothing reached the panel.

Earlier fixes in this area, all verified against the datasheet or the stock
driver: pitch is programmed in pixels rather than bytes (`addr + src_x*bpp +
src_y*src_pitch` proves the unit), the OVL and RDMA start sequences match
`OVLStart()`/`RDMAStart()`, the OVL soft reset polls `OVL_STA` before releasing,
and `INTSTA` is cleared per-bit on a level-triggered line rather than wholesale.

Two things that were *not* the cause, having been suspected and then checked:
`MUTEX0_MOD`/`MUTEX0_SOF` are programmed correctly via `mtk_mutex_add_comp()`,
and the DSI PHY node's `reg` size (`0x90`) covers the highest register the
driver touches (`0x88`).

## MT6628 — features added, then audited

Closed gaps from the original review: `EVENT_ID_SEND_DEAUTH` (the work in the tree
had DA/SA inverted and wrongly tore down the association; downstream only replies),
station statistics, real mgmt_tx ACK status, remain-on-channel, the runtime
driver/firmware ownership FSM, and reconnect after firmware recovery.

The audit of that same code found six more bugs, two of which made features
completely non-functional on hardware:

1. Reconnect never worked — recovery's `runtime_stop()` freed the saved connect
   parameters before the retry could use them.
2. Sleeping in hard IRQ — the SDIO IRQ handler drove an 8-second ownership poll.
3. ROC expiry never released the channel, blocking later requests.
4. A partial retry copy could be used to re-associate.
5. `get_station` reported the raw RCPI as dBm (128 → 128 dBm instead of −46 dBm).
6. Key-mask bookkeeping raced disconnect teardown, orphaning a key in firmware.

Also corrected: statistics were self-destroying (`read_clear=1` is right for the
downstream `SIOCSIWSTASTATS` ioctl but wrong for a passive query userspace polls).

### Fixed in the later verification round

A second DRM review arrived after the tree was declared clean.  Most of it was
wrong, but two findings were real:

- **`OVL_CON_MTX_YUV_TO_RGB` wrote the wrong field.** `OVL_CON[23:16]` is
  `HORI_BLOCK_NUM` on MT6589, so the `(6 << 16)` meant to enable a colour matrix
  was corrupting the horizontal block count.  Nothing sets that field
  deliberately, so those writes were pure noise.  Removed; YUV to RGB is implied
  by selecting a YUV `CLRFMT` on this part, and the coefficients are written
  separately by `mt6589_ovl_write_yuv_matrix()`.  The `CLRFMT` encoding is now
  documented in the source so the constant is not reintroduced.

- **The OVL SMI and RDMA SMI/output clocks were never gated.**  Both probes used
  `devm_clk_get(dev, NULL)`, which can only ever return index 0, while the OVL node
  declares an engine plus an SMI clock and the RDMA node declares engine, SMI and
  output.  Without SMI the GMC/M4U writes that set up the layer buffer may not
  reach the hardware, which presents as a blank framebuffer.  Note that
  `devm_clk_get()` cannot be worked around by repeating the ID: `clk_get()` always
  resolves index 0, so it would return the same clock N times.  `devm_clk_bulk_get_all()`
  walks the whole `clocks` property by index, which is what is now used.

- **`DSI_PHY_TIMCON2[15:8]` was being written as `DA_HS_SYNC`.**  On MT6589 that
  register has only `CLK_HS_TRAIL[31:24]`, `CLK_HS_ZERO[23:16]` and
  `CONT_DET[7:0]`, and the downstream register struct names the byte `RSV8`.  The
  driver was poking a constant 1 into a reserved byte while never programming the
  real `CONT_DET` field, which it defines and then never uses.  Fixed behind a
  driver-data flag so no other SoC changes.  `CONT_DET` is left at reset rather
  than guessed: downstream sources it from LCM parameters and there is no MT6589
  1280x800 DSI panel in the downstream tree to take the value from.

Claims from that review that were checked and found **false**:

- "CLRFMT uses wrong mt8170-style encodings" — the datasheet gives RGB888=0,
  RGB565=1, ARGB888=2, PARGB8888=3, xARGB8888=4, YUYV=8, UYVY=9, which is exactly
  what `mt6589_fmt_convert()` already emitted.
- "no MMSYS routing is programmed" — `drivers/soc/mediatek/mtk-mmsys.c:43-44` sets
  `.routes = mt6589_dispsys_routing_table`.  The earlier TDSHP revert was the
  complete fix, not half of it.

### Found by a final audit of the ownership FSM

The runtime power state machine reclaims Driver Own from the 32-bit register
helpers, but the bulk transfers call `sdio_writesb()`/`sdio_readsb()` directly
against WTDR0/WTDR1 and WRDR0/WRDR1 and so bypassed it entirely.  The practical
consequence: after the idle timer handed the chip to the firmware, a queued data
frame written to WTDR0 would not take ownership back, and the TX_DONE interrupt
that releases the resource depends on that ownership.  The frame went to a chip
the driver no longer owned, so no TX_DONE arrived and the resource was never
released — a transmit stall that only cleared on the next interrupt.  Fixed in
all six bulk transfer sites.

This is the same class as the six other bugs the audit found: the feature itself
was correct, and the defect was in how it interacted with a path added elsewhere.

### Known open, needs hardware

- `mtk_dsi.c` sets `DSI_EN` (bit 1) and `DPHY_RESET` (bit 2) in `DSI_COM_CON`,
  but the MT6589 datasheet shows that register has only bit 0 (`DSI_RESET`).  The
  real enable is `DSI_START`, which `mtk_dsi_start()` already toggles, so on
  MT6589 these are dead writes rather than a functional gap.  Deliberately not
  removed: they are inherited upstream code shared with mt2701, mt8173, mt8183,
  mt8186 and mt8188, and only the MT6589 datasheet is available here.  Removing
  them on one SoC's evidence risks the other five.
- RDMA stop does not mask `INT_ENABLE`/ack `INT_STATUS` as downstream does; with a
  level-triggered SPI this could re-fire.  Not changed: `mtk_disp_rdma.c` is shared
  across every MediaTek SoC and this pattern is upstream's.
- **DSI link rate — verified, and it is NOT a code bug.**  The mode needs
  419.022 Mbps (69837 kHz x 24 bpp / 4 lanes) and the 50-entry MT6589 MIPI TX PLL
  table's nearest entry is 416 Mbps, a 0.721% deficit, exactly as reported.  But the
  PHY driver does not blindly honour the request: `mt6589_pll_find_closest()` walks
  the table and programs the nearest entry, logging both target and achieved rate.
  More importantly, every DPHY timing field computed from `dsi->data_rate`
  (`mtk_dsi_phy_timconfig()`) evaluates to the *same integer* at 419 and 416 MHz -
  lpx 4, da_hs_prepare 4, da_hs_zero 7, clk_hs_prepare 3 - and everything else is a
  function of those, so no register is miscomputed.  Whether a 0.72% slow link
  tears on this panel is a panel characteristic, not a driver issue.
  The mode itself is self-consistent: 1416 x 822 totals give 59.9999 Hz.
- **Display power domains** — the OVL and RDMA nodes declare
  `power-domains = <&spm MT6589_POWER_DOMAIN_DIS>` but take no domain reference,
  and there is no mediatek DRM node in the tree that does.  Not changed: all
  display blocks share one domain and RDMA0's own `pm_runtime_get_sync()` powers it
  on, so the omission is inert here, and adding a reference would introduce a
  power-off path that does not exist today.
- **Panel DCS init sequence** — `boe_hx8896_a01` has no `.init_sequence`, but its
  delays (prepare 120 ms, enable 100 ms, disable 320 ms) are specific rather than
  defaults, which suggests they were measured.  aquaris-5 has no HX8896 panel
  definition at all (it is an Aquaris tree), so there is no ground truth either way.

## Deliberately not implemented

- **Roaming.** `EVENT_ID_ROAMING_STATUS` carries only `{u2Event, u2Data}` — 4
  bytes, no BSSID. The target is chosen inside the 583-line `roaming_fsm.c` from
  the driver's own scan cache, and its states hand off to the ~1100-line AIS FSM.
  A faithful port needs two interdependent FSMs plus background scan, none of
  which exist here. If wanted, do it driver-driven from the cfg80211 BSS cache
  and reuse `mt6628_cfg80211_connect()`.
- **`get_survey`.** Impossible: the MT6628 firmware has no survey/noise command.
  The whole Query set is `0x80`–`0x85`, and `GET_LINK_QUALITY` has no struct here
  and is P2P-only. Reporting invented numbers would be worse than reporting none.

Still open, each large new machinery or firmware-limited: AP, P2P, monitor, VHT,
DFS + regulatory, PMF, SAE, 802.1X.

## Review items closed by inspection

The OVL unpolled soft reset and the OVL `INTSTA`/`INTEN` ordering are **not**
defects: `mtk_disp_ovl.c` binds nine MediaTek SoCs (mt2701, mt6589, mt8167,
mt8173, mt8183, mt8183-2l, mt8192, mt8192-2l, mt8195) and this behaviour is
identical on all of them and unchanged by this port. Changing shared code for a
theoretical MT6589 concern would affect every platform and need hardware on each.

## First hardware boot — three defects the review had missed

Source review said the tree was clean. The first real boot log showed
otherwise, which is the point at which those claims were withdrawn:

- **`devm_reset_control_get(dev, "reset")` in ovl and rdma.** Given a name, the
  reset core searches the node's `reset-names` first and returns `-ENOENT` when
  there is no match — before consulting any controller. The display nodes carry a
  bare `resets = <&dispsys N>` with no `reset-names`, so all three components died:
  `error -ENOENT: failed to get reset control`. With ovl and both RDMA units gone
  there is no pipeline to drive DSI at all, which is why the panel stayed black.
  Now looked up by index, as `mtk_disp_merge.c` and `mtk_dsi.c` already do.
- **`late_probe()` returning `-EPROBE_DEFER`.** `snd_soc_card_late_probe()`'s
  return goes straight to `if (ret < 0) goto probe_end`, so a deferral abandons the
  card: `No soundcards found`, then the device parked in the deferred list. The
  accdet is not a DT dependency of the sound node, so the card now proceeds
  without it.
- **One claim of mine was wrong and was reverted.** `mt6320-accdet` was patched to
  tolerate a missing `accdet_irq` on the theory that the DT omits it. It does not:
  `mt6397-core` supplies it as a named MFD resource (`DEFINE_RES_IRQ_NAMED`), so
  there was never an `-ENXIO`. The patch was reverted rather than left in place
  on a false rationale.

A second round of review, once the display components began binding, found two
more:

- **`RDMA0_OUT_SEL` was never written.** The routing table had entries for
  RDMA0 to DBI and RDMA0 to DPI0 but none for RDMA0 to DSI0, on the stated
  assumption that DSI0 is the hardware reset default. The bootloader hands the
  panel a live DSI link, so the register arrives holding the previous kernel's
  value. Downstream writes it explicitly rather than trusting the reset value.
- **BLS was a complete no-op.** `DDP_COMPONENT_BLS` carried `funcs = NULL`, and
  every DDP hook is guarded by `if (comp->funcs && comp->funcs->x)`, so the
  block in the middle of `OVL -> COLOR -> BLS -> RDMA0 -> DSI0` was never told to
  run. Nothing claimed its reset either, so `MT6589_DISP_BLS_RST` stayed wherever
  the bootloader left it - and the DISPSYS reset is active low, so a zero there
  holds the block in reset indefinitely.
- **BLS was disabled outright.** The BLS node is claimed by
  `pwm-mt6589-disp`, which is correct: BLS is a display engine that also
  generates the backlight PWM, and the two share one enable register. But
  that driver only ever wrote `BLS_EN` as `BLS_EN_PWM_ONLY` (bit 31) or zero,
  so the scaling stage never ran and the block RDMA0 fetches from was not
  operating. It now writes `0x80010001`, the value the stock driver uses
  whenever BLS is enabled for display, and brightness zero clears only the
  PWM duty rather than the whole block.

An earlier attempt at this added a separate `mtk-disp-bls` driver. That was
wrong twice over: `mtk_disp_merge.c` drives the MT8195 merge unit with a
different register map and so could not be reused, and a second driver
matching `mediatek,mt6589-disp-pwm` would have raced the PWM driver for the
node rather than fixing it. It was reverted and the existing owner corrected.

### larb0 and larb1, and what the M4U was waiting for

larb0 and larb1 deferred without printing anything at all, which ruled out the
clock fetch and the smi link. A power-on diagnostic in
`scpsys_power_on()` then showed both failing with `-ETIMEDOUT`, which identified
the cause: they sit behind the VENC and VDE power domains.

Those two are the only MT6589 domains carrying `MTK_SCPD_KEEP_DEFAULT_OFF` that
have a live consumer, so `genpd_power_on()` actually executes for them at attach
instead of early-returning - and `PWR_STATUS` resets to `0x0007E06F`, which has
`venc[7]` and `vdec[8]` clear while `display[3]` and `isp[5]` are set.

`mtk_iommu_v1_probe()` walks every node in `mediatek,larbs` and defers on any
that has no driver, so one LARB that probes and then fails to get its domain held
the whole M4U off - and the M4U is what programs the page tables RDMA fetches
through. Nothing on this board uses VENC or VDEC, and no display component needs
those LARBs: ovl and rdma use the ports behind larb2, the dispsys one. So larb0
and larb1 are left disabled. `mtk_iommu_v1` already skips unavailable nodes, so
no driver change was needed, and disabling is strictly better than tolerating a
missing driver, since a LARB that probes and then defers cannot be skipped.

The temporary power-on diagnostic has been removed again now that this is
understood.

Two further differences from the stock driver were examined and deliberately
left alone. `RDMA_FIFO_CON` is rewritten at CRTC enable with a
pseudo-size/threshold pair where the stock driver writes it only during reset;
that is upstream shared code working as designed for nine other SoCs, and
matching the stock value would regress them. `OVL_RDMAx_MEM_GMC` is computed
from `GMC_THRESHOLD_HIGH` rather than the stock hard-coded `0x0101a06b`; these
are ultra-prefetch thresholds, the stock value is not a shift of the same
constant so the two are not directly comparable, and the field is not documented
in the data sheet. Neither plausibly stops frames, and the pipeline does not get
far enough to tell.

The lesson is the same one the MT6628 audit reached: a clean build and a
plausible commit message carry almost no assurance, and the display code was
reviewed three times without the routing or BLS gaps being noticed.

## Verified reachable, not just present

Confirmed the ported code actually runs on the target rather than merely compiling:

- Every defconfig symbol exists and maps to real code: `SND_SOC_MT6589`,
  `SND_SOC_MT6589_MT6320`, `SND_SOC_MT6320`, `SND_SOC_MT6320_ACCDET`,
  `MTK_MT6628_WLAN`, `MTK_MT6628_STP`, `MTK_MT6628_FM`, `BT_MT6628_STP`,
  `GNSS_MT6628`, `WLAN_VENDOR_MEDIATEK_MT6628` are all `=y`, and each is declared in
  a real Kconfig that selects the driver.
- The object files and their `built-in.a` archives contain the expected entry points,
  so the drivers are linked into the image rather than built and dropped.
- Device-tree status for the display pipeline: `ovl`, `rdma0`, `color`, `tdshp` and
  `dispsys` have no `status` property, so they are enabled. `dsi` is `disabled` in
  the SoC dtsi but `mt6589-lenovo-b8000.dtsi` overrides it to `okay` and attaches the
  panel. The only disabled node in that area is the BLS backlight PWM, which is
  correct: this panel is not backlit.
- The panel compatible in the DT (`boe,hx8896-a01-panel`) matches the driver's
  `of_device_id` entry exactly.
- The audio sound card (`mediatek,mt6589-mt6320-sound`) and `&mt6320_accdet` come
  from `mt6589-lenovo-blade.dtsi`, which the B8000 files include.

So the fixes in this tree are not just compiled in, they are enabled on the board.

## Process notes

- A clean build and a plausible commit message carry almost no assurance here.
  Every feature added in this session had at least one real defect, found only by
  deliberately re-reading freshly committed code. Audit new code against the
  disconnect/recovery lifecycle specifically — that is where the bugs cluster.
- Review-agent findings still need verifying: one agent got a command's parameters
  right and the returned signal's units wrong.
- Keep review sub-agents single-level. One nested sub-agents and burned its budget
  waiting on them, and its report never arrived.