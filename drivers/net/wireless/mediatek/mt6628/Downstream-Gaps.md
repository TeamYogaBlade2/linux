# MT6628 WLAN — downstream gap analysis

Scope: what the MT6628 WLAN driver in this tree implements, and what it does not,
relative to the MediaTek downstream driver in `./aquaris-5`
(`mediatek/kernel/drivers/combo/drv_wlan/mt6628/wlan`).

Everything below was checked against the downstream source while writing it.
Where a downstream symbol is quoted, the file is given so it can be re-checked.

**Nothing here has been validated on hardware.** This driver has never been
run on the tablet.

---

## 1. What is implemented

### cfg80211 surface

`drivers/net/wireless/mediatek/mt6628/mt6628-wlan-cfg80211.c`:

```
.scan / .abort_scan
.connect / .disconnect
.add_key / .del_key
.set_power_mgmt
.mgmt_tx
.get_station
.remain_on_channel / .cancel_remain_on_channel
```

`interface_modes = BIT(NL80211_IFTYPE_STATION)` — station only.

### Capabilities

| Area | State |
|---|---|
| Bands | 2.4 GHz (ch 1–14) and 5 GHz (34–48, 149–173) |
| PHY | 11b/g/a/n, HT20 and HT40 (STA), no VHT |
| Ciphers | WEP40, WEP104, TKIP, CCMP |
| Auth | Open, Shared Key, WPA-PSK, WPA2-PSK |
| Max SSIDs / IE length | 4 / 600 bytes |
| Firmware interface | SDIO func 1, 512-byte blocks, HIF full-MAC TX/RX |

### Commands and events

Ten commands are issued (`mtk-wlan-hif.h`) — `SCAN_REQ_V2`, `POWER_SAVE_MODE`,
`ADD_REMOVE_KEY`, `SCAN_CANCEL`, `BSS_ACTIVATE_CTRL`, `SET_BSS_INFO`,
`UPDATE_STA_RECORD`, `REMOVE_STA_RECORD`, `CH_PRIVILEGE`,
`GET_STA_STATISTICS` — plus `BASIC_CONFIG` at probe.

All eight event IDs the driver defines are consumed:

```
CMD_RESULT, BASIC_CONFIG, SCAN_DONE, TX_DONE,
CH_PRIVILEGE, BSS_BEACON_TIMEOUT, SEND_DEAUTH, STA_STATISTICS
```

### Connection lifecycle

Auth → associate FSM with CH_PRIVILEGE, STA-REC states 1→2→3, BSS_INFO,
deauth/disassoc, beacon-loss timeout, peer deauth/disassoc, firmware
`SEND_DEAUTH`, key install/teardown tracked per index, and firmware recovery
(reset → Driver Own → reload → runtime restart → reconnect).

---

## 2. Gaps

### 2.1 Blocked by the firmware, not by the port

#### `get_survey()` — impossible on this chip

The complete downstream *Query* command set is six IDs
(`include/nic_cmd_event.h`):

```
CMD_ID_GET_NIC_CAPABILITY   0x80
CMD_ID_GET_LINK_QUALITY    0x81
CMD_ID_GET_STATISTICS      0x82
CMD_ID_GET_CONNECTION_STATUS 0x83
CMD_ID_GET_ASSOC_INFO      0x84   (marked obsolete)
CMD_ID_GET_STA_STATISTICS  0x85
```

There is **no** survey, channel-usage or noise command. `GET_LINK_QUALITY` has
no `CMD_GET_LINK_QUALITY_T` structure anywhere in this tree and is only ever
issued from `common/wlan_p2p.c`, i.e. it is a P2P facility rather than a
per-channel noise source. Grepping the MT6628 tree for noise counters finds
nothing in the firmware-facing structures.

Implementing the cfg80211 op would mean publishing numbers the hardware never
produced. `iw survey` output would look measured while being fiction, so the op
is left unimplemented.

#### Roaming — `EVENT_ID_ROAMING_STATUS` cannot be used as-is (resolved driver-side)

`CFG_SUPPORT_ROAMING` is 1 for MT6628 (`include/config.h:1462`) and
`nic_rx.c:2113` does dispatch `EVENT_ID_ROAMING_STATUS`, so this is live
downstream code, not a dead event.

The problem is what the event carries.
`include/mgmt/roaming_fsm.h:91`:

```c
typedef struct _ROAMING_PARAM_T {
    UINT_16 u2Event;
    UINT_16 u2Data;
} ROAMING_PARAM_T;
```

Four bytes: an event code (`START`/`DISCOVERY`/`ROAM`/`FAIL`/`ABORT`) and a
16-bit payload. No BSSID, no candidate list.

The target AP is chosen inside `mgmt/roaming_fsm.c` (583 lines) from the
driver's own scan-result cache — the only cache access in the whole file is
`scanSearchBssDescByBssid()` at line 420, used just to refresh the current BSS's
RCPI. Its `DISCOVERY` and `ROAM` states then hand off to the AIS connection FSM
in `mgmt/ais_fsm.c`, which runs its own reconnection sequence.

A faithful port therefore means porting two interdependent FSMs plus a
background-scan request path and a BSS descriptor pool — none of which exist in
this driver, which keeps scan results in cfg80211's BSS list instead
(`cfg80211_inform_bss_frame()`).

**Resolution:** the firmware FSM was not ported, and the driver-driven approach
this section recommends is what now runs — see `mt6628_roam_start()`. On beacon
loss it picks a candidate from the cfg80211 BSS cache, requiring a 10 dB gain
over the AP just lost and excluding that AP outright, then reuses the existing
`mt6628_cfg80211_connect()` with the saved IE and key material. The RCPI
collected during scanning is what cfg80211 turned into `bss->signal` in the
first place, so no separate signal path was needed.

#### AWB / second I2S / hardware digital gain — no producer on this board

These three form one chain whose only purpose downstream is carrying FM radio
audio:

- `mt_soc_fm_i2s2.c` is the **only** caller of `SetHwDigitalGain()` /
  `SetHwDigitalGainMode()` anywhere in the tree.
- The AWB platform's only machine-driver DAI link is `FM_I2S2_IN`
  (`mt_soc_machine.c:392-400`), whose codec side is
  `MT_SOC_CODEC_FMI2S2RXDAI_NAME`.

In this tree the FM radio is `drivers/net/wireless/mediatek/mt6628/mtk-fm.c`
(built, `CONFIG_MTK_MT6628_FM=y`) but it registers only

```c
fm->vdev.device_caps = V4L2_CAP_RADIO |
                       V4L2_CAP_TUNER |
                       V4L2_CAP_HW_FREQ_SEEK;
```

— no `V4L2_CAP_AUDIO`, no PCM stream — and it is entirely STP-based with no I2S
wiring at all. Adding the chain would produce capture endpoints that can never
be opened.

#### DL2 — dead upstream too

The only reference to `MEM_DL2` anywhere in the downstream kernel tree is
`mt_soc_pcm_afe.c:573`, which formats DL1's 32-bit data. It is never enabled as
a path.

### 2.2 Voice subsystem — out of scope for a Wi-Fi tablet

`mgmt/mt_soc_voice.c` (525 lines) plus `PCM2_VOICE`, modem PCM interfaces,
DAI / MOD_DAI and sidetone. These are Android speech-subsystem facilities tied
to the baseband; the modem-facing handshake has no meaning without that stack.

Present in the firmware interface (`include/nic_cmd_event.h` /
`AudDrv_Afe.h`) but unused here:

```
DAI_BASE / DAI_END / DAI_CUR      DAI_MOD / MOD_DAI
MODEM_PCM_*  SIDETONE_*  HW digital gain (GAIN1/GAIN2)
PCM2_VOICE
```

### 2.3 Not implemented, and genuinely absent from the port

- **AP, P2P, monitor modes.** `interface_modes` is STATION only. Downstream
  carries the full P2P group (`mt_soc_p2p_*.c`, `mt_soc_fm_i2s2.c`,
  `common/wlan_p2p.c`); none of it is ported. Note P2P was nonetheless the most
  plausible consumer of the `CH_REQ_TYPE_P2P_LISTEN` request type that
  remain-on-channel now uses, so the two overlap in register usage.
- **VHT / 802.11ac.** No VHT capability advertised, and no firmware control
  beyond 11AN.
- **Radar detection (DFS master function).** This is the part that genuinely
  cannot work: the firmware RDD command and event (`CMD_ID_SET_RDD_CH`,
  `EVENT_ID_UPDATE_RDD_STATUS`) exist downstream but are not ported, and
  `CFG_SUPPORT_RDD_TEST_MODE` is `0` (`include/config.h:1397`). There is no
  CAC and no radar detection in this driver, so the consequence is enforced
  rather than left to configuration — see §2.6. DFS channels are absent from
  the 5 GHz table and `mt6628_reg_dfs_guard()` disables any that appear.
- **No country code.** The driver ships no `regulatory_hint()` and no custom
  regulatory domain, so the active domain is whatever cfg80211 and CRDA say.
  No DT property was added for this: there is no verifiable country for this
  tablet, and inventing one would be worse than letting the board fall back.
- **PMF / 802.11w.** `connect()` rejects anything but `NL80211_MFP_NO`.
  Downstream has `CFG_SUPPORT_802_11W` code but it is `0` in the product
  configuration.
- **SAE / WPA3.** No MT6628 SAE implementation was found in the downstream
  tree.
- **802.1X enterprise.** Only `WLAN_AKM_SUITE_PSK` is accepted. Note the HIF
  *can* carry EAPOL frames (`ETH_P_PAE` sets the 1X bit in the Ethernet TX
  path), but cfg80211 connection negotiation has no enterprise support.

### 2.4 Unhandled events

The downstream dispatch table (`nic_rx.c`) handles ~40 events. Events the
firmware may raise that this driver does not act on:

```
STA_AGING_TIMEOUT            AP/P2P oriented
BSS_ABSENCE_PRESENCE         BSS monitoring
RX_FLUSH                     invalidation on BSS change
STA_CHANGE_PS_MODE           peer power-save notification
AP_OBSS_STATUS / UPDATE_NOA_PARAMS   AP/P2P oriented
SLEEPY_NOTIFY / FW_SLEEPY_NOTIFY     firmware sleep state (see below)
SEC_CHECK_RSP / SW_DBG_CTRL / DUMP_MEM / RX_ERR   debug and test paths
UPDATE_RDD_STATUS / UPDATE_BWCS_STATUS / UPDATE_BCM_DEBUG
BUILD_DATE_CODE
STA_STATISTICS_UPDATE
```

Unhandled events are discarded: anything the event handler does not claim is
freed immediately. This used to be a 256-entry queue
(`wl->async_event_queue`) that nothing ever drained, which both leaked every
such event and — because `mt6628_wlan_give_firmware_own()` treats a non-empty
queue as outstanding traffic — permanently prevented the radio from reaching
its idle state. The queues have been removed. If an extension point is ever
wanted here, give the queue a real consumer first.

#### RX Block Ack sessions — counted, not tracked

`RX_ADDBA` (0x11) and `RX_DELBA` (0x12) used to be listed above. They are now
consumed — decoded, counted and logged — but deliberately without host-side BA
state. The enum annotates both "(obsolete)"
(`include/nic_cmd_event.h:779-780`); that comment is wrong.
`nic_rx.c:1715-1722` dispatches them to `qmHandleEventRxAddBa()` /
`qmHandleEventRxDelBa()`, and `include/config.h:1158` has
`CFG_RX_REORDERING_ENABLED 1`. So this is live downstream code. The event bodies
are:

```c
/* include/nic/que_mgt.h:532 — EVENT_RX_ADDBA_T, 8 bytes after the header */
u8     ucStaRecIdx;
u8     ucDialogToken;
__le16 u2BAParameterSet;     /* BA policy, TID, buffer size */
__le16 u2BATimeoutValue;
__le16 u2BAStartSeqCtrl;     /* SSN */

/* include/nic/que_mgt.h:551 — EVENT_RX_DELBA_T, 2 bytes after the header */
u8 ucStaRecIdx;
u8 ucTid;
```

Downstream feeds those into a bounded per-station/per-TID table
(`RX_BA_ENTRY_T`, `include/nic/que_mgt.h:441`; `CFG_NUM_OF_RX_BA_AGREEMENTS`
= 8, `include/config.h:1154`) through `qmAddRxBaEntry()` (`nic/que_mgt.c:3468`)
and `qmDelRxBaEntry()` (`:3550`).

**This driver keeps no such table, on purpose.** The table is not consumed for
its own sake — its only reader is the host RX reorder path:
`qmHandleRxPackets()` dispatches to `qmProcessPktWithReordering()`
(`nic/que_mgt.c:2762`, defined `:2795`) when the per-packet HIF reorder flag is
set, and to `qmProcessBarFrame()` for Block Ack Request frames. Nothing else
reads `aprRxReorderParamRefTbl` except a debug dump in `mgmt/swcr.c:392`.

That reorder path is unreachable here, and is not something that could simply
be re-enabled:

- The flag comes from `uc80211_Reorder_PAL_TCL & HIF_RX_HDR_DO_REORDER`
  (`include/nic/hif_rx.h:170`), but the downstream code that decodes that byte
  into `HIF_RX_HDR_FLAG_DO_REORDERING`, `u2SSN` and `ucTid` sits inside an
  `#if 0` block in the **downstream driver itself** (`nic_rx.c:1088-1115`). So
  `prSwRfb->ucTid` and `u2SSN` are never set from the wire, and
  `qmProcessPktWithReordering()` would key every lookup on a stale zero TID.
- cfg80211 has no reorder-window concept. The 802.11 frames reach
  `mt6628_napi_poll()` already reassembled out of the SDIO descriptor queue
  (`mt6628-wlan-runtime.c:238`), and `mt6628_cfg80211_rx_mgmt()` reads the
  management frame straight from the HIF payload.
- Window start/end/size would therefore be a table written on every event and
  never read — state implying a capability the driver does not have, and that
  would rot silently across disconnects.

The events are still worth consuming, because they are the only host-visible
handle on what the firmware agreed to. `mt6628_wlan_handle_rx_ba_event()` in
`mt6628-wlan-control.c` decodes both with the firmware's own field masks rather
than a guess (`include/nic/mac.h:681-684` and `:465`):

```c
tid       = (u2BAParameterSet & BITS(2,5))  >> 2;   /* TID         */
win_size  = (u2BAParameterSet & BITS(6,15)) >> 6;   /* buffer size */
win_start = u2BAStartSeqCtrl >> 4;                  /* SSC Start SN */
```

and logs them at debug level, counting them in `wl->rx_addba_events` /
`wl->rx_delba_events`. An out-of-range `ucStaRecIdx` discards the event, as
downstream does (`nic/que_mgt.c:3363-3369`); the bound is `CFG_STA_REC_NUM` = 20
(`include/nic/wlan_def.h:287`), which also rejects the two reserved indices
`STA_REC_INDEX_BMCAST` / `STA_REC_INDEX_NOT_FOUND`, 0xff and 0xfe
(`include/mgmt/cnm_mem.h:501-502`).

If host-side reordering is ever wanted, a reorder *flag* has to be decoded out of
the HIF RX header first. That is separate work, not an extension of this one.

### 2.5 Behavioural notes

- **Roaming:** driver-driven, not firmware-driven. `EVENT_ID_ROAMING_STATUS`
  carries no BSSID, so the firmware path is unusable here; instead the
  driver walks the cfg80211 BSS cache on beacon loss and reconnects to the
  strongest same-SSID entry that beats the AP just lost by 10 dB, reusing
  the saved IE and key material. One attempt per link loss, budget refilled
  only by a successful association; a failed roam falls back to the bounded
  reconnect path. It does not roam to a *different* network, and it does
  not carry traffic across the move (cfg80211 is told the link dropped
  first), so userspace may still see a brief disconnect and reconnect.
- **Regulatory:** CRDA-governed. The channel tables describe what the hardware
  can do; the active domain decides what is usable. Country IEs are ignored
  and there is no country code. See §2.6.
- **Suspend/resume:** implemented. Suspend cancels the idle countdown and hands
  the chip to the firmware, refusing the suspend (`-EBUSY`) if traffic is still
  in flight. `freeze`/`thaw` share that path and `restore` reclaims Driver Own.
  The firmware is not told to enter a separate low-power mode — this chip has no
  such command — so the radio simply ends up firmware-owned, which is the same
  state the runtime idle path already uses.
- **Concurrency:** the command engine is single-outstanding (`cmd_mutex`), so
  only one command transaction is in flight at a time.

---

## 2.6 Regulatory handling

### What is claimed, and what is not

The driver sets exactly one regulatory flag, ships no custom regulatory domain
and makes no `regulatory_hint()`. The channel set and the transmit power are
therefore owned by cfg80211 and CRDA, not by this driver:

```c
wl->wiphy->regulatory_flags = REGULATORY_COUNTRY_IE_IGNORE;
```

- **`REGULATORY_COUNTRY_IE_IGNORE`** — this board carries no verifiable
  country, and `mt6628_cfg80211_mgmt_handler()` feeds every ESS beacon it sees
  to `cfg80211_inform_bss_frame()`, which in turn raises a regulatory hint
  (`net/wireless/scan.c:2333`). Without this flag an AP broadcasting nearby
  gets to define what this radio may transmit.
- **Beacon hints stay enabled, deliberately.** They cannot compromise DFS
  safety: `regulatory_hint_found_beacon()` returns early on an
  `IEEE80211_CHAN_RADAR` channel (`net/wireless/reg.c:3658`), and on 2.4 GHz it
  is limited to channels 12/13/14 by `freq_is_chan_12_13_14()`. But they are
  what keeps the 5 GHz band usable whenever no `regulatory.db` is present —
  see the behaviour notes below.
- **`REGULATORY_STRICT_REG` is not set.** It is only meaningful alongside a
  driver-set `wiphy->regd`; with none, `ignore_reg_update()` discards every
  subsequent regulatory change (`net/wireless/reg.c:2113`), which would pin the
  radio to the world fallback even after a regdb or user hint arrives.

`mt6628_reg_notifier()` re-runs the DFS guard on every domain change; there is
nothing else to reprogram, because cfg80211 has already written the resulting
flags and `max_reg_power` into the tables by then.

### DFS

The 5 GHz table contains channels 34–48 and 149–173 only. None of those are
DFS, so **no DFS channel is reachable today** — the failure mode the summary
used to warn about ("a 5 GHz channel on a DFS channel would be used without
CAC") cannot occur. Note that `cfg80211_chandef_dfs_required()` returns `0`
unconditionally for station mode (`net/wireless/chan.c:815`), so merely
*flagging* a channel `IEEE80211_CHAN_RADAR` would not have made a station
associate to it safely — there is no CAC in this driver to wait for either way.

Downstream does list channels 52–144 (`gl_init.c:745-751`), all with
`.flags = 0`, so the hardware can tune them. They are omitted rather than
added-and-disabled, since an entry with no usable configuration is not a
capability. To keep that from silently regressing, `mt6628_reg_dfs_guard()`
walks the 5 GHz band at registration and from the regulatory notifier, and sets
`IEEE80211_CHAN_DISABLED` on anything in 5260–5725 MHz with a warning. Adding
a DFS channel to the table later therefore fails loudly instead of turning
into a channel this radio would use without CAC.

### Per-channel flags

Only one flag is asserted in the table, on channel 14:

| Channel | Freq | Flags | Why |
|---|---|---|---|
| 1–13 | 2412–2472 MHz | none | see below |
| 14 | 2484 MHz | `NO_HT40`, `NO_OFDM` | 802.11b only — a property of the channel, not of any domain |

Channels 12 and 13 are present because the hardware can tune them, but are left
unflagged on purpose: whether they may be scanned on or radiated on is a
domain decision. `reg_process_ht_flags()` derives their `NO_HT40PLUS` /
`NO_HT40MINUS` from the active domain's bandwidth and adjacent-channel rules
(`net/wireless/reg.c:2305`), and cfg80211 recomputes those at
`wiphy_register()`, so a hard-coded guess would be overwritten.

### Behaviour with and without `regulatory.db`

`wiphy_register()` calls `wiphy_regulatory_register()` unconditionally
(`net/wireless/core.c:1140`), so the world fallback is applied either way.

- **No regdb (this board).** cfg80211 uses `world_regdom`
  (`net/wireless/reg.c:233`): 2.4 GHz 1–11 usable; 12/13 `NO_IR`; 14
  `NO_IR | NO_OFDM`; all 5 GHz `NO_IR`. Beacon hints then lift `NO_IR` on the
  channels where this driver actually reports a beacon, so 2.4 GHz 1–11 and
  the 5 GHz channels in use by a nearby AP become connectable — 5 GHz keeps
  working on a board with no domain, which is the common case here. Channels
  12/13/14 are unaffected either way: hints never apply to them on 2.4 GHz
  unless the domain itself says so.
- **With regdb / a user hint.** The domain applies normally: `max_reg_power`
  clamps every channel, DFS channels a domain permits are still absent from
  the table and stay disabled, and channels a domain forbids get
  `IEEE80211_CHAN_DISABLED` from `handle_channel()`.

`max_power` in the tables (20 dBm 2.4 GHz, 30 dBm 5 GHz) is the hardware
ceiling; cfg80211 reduces it to the domain's `max_eirp`. The driver does not
program transmit power to the firmware at all.

---


## 3. Summary

| Area | State |
|---|---|
| STA scan / connect / data path | complete |
| WEP, WPA-PSK, WPA2-PSK | complete |
| HT20/HT40, 2.4 + 5 GHz | complete |
| Station statistics | complete (signal, TX packets, retries) |
| Remain-on-channel | complete |
| Ownership power management | complete |
| System suspend / resume | complete (idle hand-off driven from `.drv.pm`) |
| Firmware recovery + reconnect | complete |
| `get_survey` | impossible — no firmware command |
| Roaming | driver-driven from the BSS cache (not the firmware FSM) |
| AWB / 2nd I2S / HW gain | no producer (FM audio only) |
| DL2 | dead upstream too |
| Voice / modem PCM / DAI / sidetone | out of scope |
| AP / P2P / monitor | not implemented |
| VHT | not implemented |
| DFS (radar detection / CAC) | impossible — no firmware RDD support; DFS channels are disabled |
| Regulatory domain | CRDA-governed; country IEs ignored, no country code |
| PMF, SAE, 802.1X | not implemented |
| `async_event_queue` | removed — unhandled events are freed, not queued |

The driver is a complete STA-mode full-MAC driver. Everything in the "blocked"
and "out of scope" rows is absent for a reason other than the port falling
short; the genuinely missing functionality is AP/P2P/monitor, VHT and the
security extensions. Radar detection is enforced-absent rather than missing:
DFS channels cannot be used (§2.6).
---

## Bring-up note

The WLAN attaches as **SDIO function 1** and STP (Bluetooth/FM/GNSS) as
function 2 on the same controller. Both function nodes must exist in the
device tree or the function is never enumerated and the radio never
probes. A missing function 1 node produces no MMC or SDIO messages at
all in the boot log, which is easy to mistake for a driver hang.
