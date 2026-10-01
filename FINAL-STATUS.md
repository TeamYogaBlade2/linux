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

Claims from that review that were checked and found **false**:

- "CLRFMT uses wrong mt8170-style encodings" — the datasheet gives RGB888=0,
  RGB565=1, ARGB888=2, PARGB8888=3, xARGB8888=4, YUYV=8, UYVY=9, which is exactly
  what `mt6589_fmt_convert()` already emitted.
- "no MMSYS routing is programmed" — `drivers/soc/mediatek/mtk-mmsys.c:43-44` sets
  `.routes = mt6589_dispsys_routing_table`.  The earlier TDSHP revert was the
  complete fix, not half of it.

### Known open, needs hardware

- `mtk_dsi.c` sets `DSI_EN` (bit 1) and `DPHY_RESET` (bit 2) in `DSI_COM_CON`,
  but the MT6589 datasheet shows that register has only bit 0 (`DSI_RESET`).  The
  real enable is `DSI_START`, which `mtk_dsi_start()` already toggles, so on
  MT6589 these are dead writes rather than a functional gap.  Deliberately not
  removed: they are inherited upstream code shared with mt2701, mt8173, mt8183,
  mt8186 and mt8188, and only the MT6589 datasheet is available here.  Removing
  them on one SoC's evidence risks the other five.
- `DSI_PHY_TIMCON2` / `DA_HS_SYNC` reportedly lands in a reserved byte on MT6589 —
  not verified either way.
- RDMA stop does not mask `INT_ENABLE`/ack `INT_STATUS` as downstream does; with a
  level-triggered SPI this could re-fire.  Not changed: `mtk_disp_rdma.c` is shared
  across every MediaTek SoC and this pattern is upstream's.
- DSI link rate computes to 419.022 Mbps and the MT6589 PLL table rounds to 416 Mbps
  (~0.72% deficit), which could cause a 60 Hz tear.  The BOE HX8896-A01 panel also
  has no DCS init sequence in the driver.

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

There are therefore no known outstanding defects in the tree.

## Process notes

- A clean build and a plausible commit message carry almost no assurance here.
  Every feature added in this session had at least one real defect, found only by
  deliberately re-reading freshly committed code. Audit new code against the
  disconnect/recovery lifecycle specifically — that is where the bugs cluster.
- Review-agent findings still need verifying: one agent got a command's parameters
  right and the returned signal's units wrong.
- Keep review sub-agents single-level. One nested sub-agents and burned its budget
  waiting on them, and its report never arrived.