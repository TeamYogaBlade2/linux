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

### Station statistics: what the firmware actually returns

`get_station()` reports signal, `tx_packets`, `tx_retries` and now `tx_failed`.
That is the whole of what can honestly be published, and it is worth saying why,
because each obvious candidate turned out to be unavailable:

| Wanted | Available? | Why |
|---|---|---|
| `tx_failed` | **yes, now added** | `EVENT_ID_STA_STATISTICS.u4TxFailCount` |
| `rx_bytes`, `tx_bytes` | no | the event carries no byte counter of any kind |
| link quality | no | see below |
| `connected_time` | no | not tracked host-side, and the event has no field for it |
| tx/rx bitrate | no | no field to put it in, see below |

**No byte counters exist.** `EVENT_ID_STA_STATISTICS_T`
(`nic_cmd_event.h:1690-1724`) carries `u4TxCount`, `u4TxFailCount`,
`u4TxLifeTimeoutCount` and `u4TxDoneAirTime` — all packet or airtime counts, none
of them bytes. Searching the whole downstream tree for a byte counter turned up
only `u2TxByteCount_UserPriority`, which is a HIF *transmit descriptor* field
describing the packet about to be sent (`nic_cmd_event.h:830`), not a statistic.
No query command in `ENUM_CMD_ID_T` (`nic_cmd_event.h:723-760`) returns one for a
station. So `rx_bytes`/`tx_bytes` are left unfilled rather than approximated.

**`tx_failed` is a genuinely separate counter, not a restatement of
`tx_retries`.** The firmware keeps `u4TxFailCount` (retries exhausted, never
acknowledged) and `u4TxLifeTimeoutCount` (aged out in the queue) apart, and the
vendor's own reader treats them as independent — `gl_cfg80211.c:1623` adds them
into a single error total rather than substituting one for the other, and
`:1668-1669` exports them as two separate netlink attributes. `tx_retries` keeps
its existing `tx_life_timeout_count` mapping and `tx_failed` takes
`tx_fail_count`.

**No link quality is published, and the obvious candidate is a trap.**
`EVENT_ID_STA_STATISTICS_T` does have a `ucLinkQuality` byte, and it would be a
one-line change to report it. It should not be. Grepping the entire vendor tree
for `ucLinkQuality` returns only its definition — nothing reads it out of this
event, anywhere, ever. The field the vendor *does* consume is
`EVENT_LINK_QUALITY.cLinkQuality`, which arrives via a different command
(`CMD_ID_GET_LINK_QUALITY`) and whose scale is likewise undocumented. Publishing a
byte whose scale and provenance are unknown would hand userspace a plausible-
looking but meaningless 0-100 number.

The vendor *does* compute a real 0-100 link score
(`gl_cfg80211.c:1621-1662`), and it is a good metric — but it cannot be
reproduced here. Its two main inputs, `u4TxTotalCount` and
`u4TxExceedThresholdCount`, are marked **"From driver"** in
`PARAM_GET_STA_STATISTICS` (`wlan_lib.h:576-583`) and are filled from
`prStaRec->u4TotalTxPktsNumber` and `prStaRec->u4ThresholdCounter`
(`wlan_lib.c:5732-5733`), which the downstream driver maintains in its own TX
completion hook. That hook does not exist in this port, so reimplementing the
score would mean inventing a packet-time threshold the vendor defines elsewhere.

`ucPer` (base 128) and `u4PhyMode` are likewise reported verbatim to userspace by
the vendor without ever being interpreted, so there is no scale to publish.

**The bitrate has nowhere to go.** `u2LinkSpeed` is in units of 0.5 Mbit/s — the
vendor multiplies by 5000 for bps (`nic_cmd_event.c:598`) — but this cfg80211
vintage's `struct rate_info` (`include/net/cfg80211.h`) has no bitrate member, so
there is no honest destination for it. Deriving a value from `u4PhyMode` would
mean writing an MCS/BW-to-rate table the vendor does not ship.

One caveat applies to all of it: `CMD_ID_GET_STA_STATISTICS` is only sent when
the firmware advertises `COMPILE_FLAG0_GET_STA_LINK_STATUS`
(`wlan_lib.c:5762`, flag defined `config.h:1521`), and this tree has no way to
read `u4FwCompileFlag0`. On a firmware that does not advertise it, the command
never completes, `get_station()` returns `-EOPNOTSUPP`, and none of the above is
published at all.

### Unhandled events that remain unhandled, with the reason

The list at the top of this section is a list of what the firmware *may* raise,
not of what this driver should act on. Each remaining entry was checked against
the MT6628 downstream dispatch (`nic_rx.c:1636-2220`) and the MT6628-specific
`config.h`; the ones that survive are genuinely not actionable here:

- **`EVENT_ID_STA_AGING_TIMEOUT` (0x21)** is an **AP-mode** event and cannot
  apply to a station-only driver. Downstream removes the STA_REC from the AP's
  client list (`nic_rx.c:2070-2097` → `bssRemoveStaRecFromClientList()`,
  `bss.c:2037`), which requires a `rStaRecOfClientList` that the vendor itself
  documents as "For IBSS/AP Mode, all known STAs in current BSS"
  (`adapter.h:774`). There is no client list to remove anything from, and the
  event cannot fire: the only peer this firmware has is the AP it is associated
  with, and from the AP's perspective that is not an aged client.
  It was also suggested this be delivered via `cfg80211_inform_bss_frame()`:
  that function takes a **beacon or probe response** (`net/wireless/scan.c:3253`),
  it has no station-timeout concept at all, and cfg80211 ages *BSS entries* purely
  by time (`cfg80211_bss_expire()`, `scan.c:1393`) — never by a firmware event.
  Feeding it this event would be meaningless. So it stays unhandled, by design.
- **`EVENT_ID_RX_FLUSH` (0x16)** and **`EVENT_ID_SEC_CHECK_RSP` (0x22)** have
  **no handler at all** in the MT6628 downstream dispatch — they are declared in
  `nic_cmd_event.h:784` and `:797` and appear in no `case` label anywhere in the
  tree. There is no downstream semantics to reproduce, and guessing at one would
  be inventing it. Left unhandled.
- **`EVENT_ID_UPDATE_NOA_PARAMS` (0x1C)** and **`EVENT_ID_AP_OBSS_STATUS` (0x1D)**
  are P2P/AP concepts. Both downstream handlers are behind `if
  (prAdapter->fgIsP2PRegistered)` (`nic_rx.c:2052`, `:2105`), and the NOA body is
  only accepted when `ucNetTypeIndex == NETWORK_TYPE_P2P_INDEX`. This radio is
  STA-only; there is no P2P interface for them to describe. (Note that
  `CFG_ENABLE_WIFI_DIRECT` is 1 in this build — `config.h:1339` — but P2P is not
  implemented by this driver, so `fgIsP2PRegistered` can never be true.) The
  remain-on-channel path in this driver reports *the driver's own* window to
  cfg80211 and is never fed by the firmware, so there is nothing here to forward.
- **`EVENT_ID_STA_CHANGE_PS_MODE` (0x1A)** only updates
  `prStaRec->fgIsInPS` and the per-STA free quota (`que_mgt.c:4625-4660`).
  In station mode there is exactly one STA_REC — the AP — and the free quota it
  adjusts is a queue-reservation mechanism for servicing *many* stations. This
  driver has no per-STA PS state and no quota table to mirror the value into, so
  there is nothing for the event to change.
- **`EVENT_ID_BSS_ABSENCE_PRESENCE` (0x19)** sets
  `prBssInfo->fgIsNetAbsent` and `ucBssFreeQuota` (`que_mgt.c:4580-4611`), which
  downstream then uses to *withhold transmit queue space* while the BSS is absent
  (`que_mgt.c:4466-4477`, and six further call sites). This driver does no
  transmit queue accounting in the firmware — the SDIO FIFO cap in `mtk-stp.c`
  is a transport concern, unaware of BSS presence — so mirroring the flag would
  add state nothing reads. Link loss is already handled directly, through
  `EVENT_ID_BSS_BEACON_TIMEOUT` and the driver-driven roam in §2.5.
- **`EVENT_ID_SLEEPY_NOTIFY` (0x0e)** only sets `fgWiFiInSleepyState`, which the
  vendor reads to gate its *own* power-management state machine
  (`pwr_mgt.h:129`, `wlan_lib.c:5213`). This driver has no such state machine to
  gate — its runtime idle path is driven by the WMT Driver Own/Firmware Own
  handshake, and the firmware's sleepy flag has no equivalent host-side meaning.
  Recording it would add state nothing reads.
- The remaining entries (debug, test and build-date paths: `SW_DBG_CTRL`,
  `DUMP_MEM`, `RX_ERR`, `UPDATE_RDD_STATUS`, `UPDATE_BWCS_STATUS`,
  `UPDATE_BCM_DEBUG`, `BUILD_DATE_CODE`, `STA_STATISTICS_UPDATE`) have either no
  downstream handler or handlers behind `CFG_SUPPORT_RDD_TEST_MODE`, which is `0`
  in this configuration (`config.h:1397`).

So the driver now acts on the events that have a well-defined kernel-side meaning
in station mode — beacon timeout, deauth, Block Ack notification, scan done — and
deliberately drops the rest. The dispatcher frees what it does not claim, so an
unhandled event costs one `kfree_skb()` and nothing else.

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

---

# MT6628 FM, STP and GNSS — downstream gap analysis

Companion to the WLAN analysis above, covering

- `drivers/net/wireless/mediatek/mt6628/mtk-fm.c` (the V4L2 radio device)
- `drivers/net/wireless/mediatek/mt6628/mtk-stp.c` (SDIO function 2 transport)
- `drivers/gnss/gnss-mt6628.c` (the GNSS relay)

Downstream references are to `./aquaris-5`:

| This tree | Downstream |
|---|---|
| `mtk-fm.c` | `mediatek/kernel/drivers/fmradio/mt6628/` and `mediatek/kernel/drivers/fmradio/core/` |
| `mtk-stp.c` | `mediatek/kernel/drivers/combo/common/core/stp_core.c` |
| `gnss-mt6628.c` | `mediatek/kernel/drivers/conn_soc/common/linux/pub/stp_chrdev_gps.c` |

**None of these three drivers has been validated on hardware.** Neither the
operator nor the project has ever run them on the tablet: they compile, they
were written against the downstream source, and that is the entire extent of
the evidence behind them. Every register field, opcode and scaling factor used
below was taken from the vendor source and is cited so it can be re-checked, but
no claim here has been confirmed against a real chip. The same caveat that
applies to the WLAN section applies with equal force here.

## FM radio — what is implemented

The V4L2 surface is the whole of it. What exists today:

| Area | Surface |
|---|---|
| Tuning | `VIDIOC_G/S_FREQUENCY`, `VIDIOC_ENUM_FREQ_BANDS` |
| Searching | `VIDIOC_S_HW_FREQ_SEEK`, 50/100/200 kHz spacing |
| Stereo / mono | `tuner->audmode` and `tuner->rxsubchans` |
| Signal level | `tuner->signal`, post-seek CQI else live FM_RSSI_IND |
| RDS | `V4L2_CID_RDS_RECEPTION` plus six decoded `V4L2_CID_RDS_RX_*` controls |
| Mute | `V4L2_CID_AUDIO_MUTE` to FM_MAIN_CTRL[5] |
| Volume | `V4L2_CID_AUDIO_VOLUME` to the chip's 16-entry gain table in 0x7d |
| De-emphasis | `V4L2_CID_TUNE_DEEMPHASIS` to FM_MAIN_CG2_CTRL[12] |

The power-up sequence follows the downstream one including the ROM-version
probe, and the patch and coefficient download policy matches
`mt6628/pub/mt6628_fm_lib.c:294-320`. Mute, volume, de-emphasis and force-mono
are all cached and re-applied by `mtk_fm_open()` after power-up, because the
power-down sequence resets the registers that hold them.

The following is **still** worth reading as a caveat rather than as
progress: none of this has been run on a chip. Every register field, opcode
and scaling constant below was taken from the vendor source and is cited so it
can be re-checked, but the driver has never been probed.

Three deliberate choices:

- `tuner->afc` is always 0. The MT6628 has no AFC: downstream only uses
  `AFC_ON` to select an alternative power-on value
  (`mt6628/inc/mt6628_fm.h:48-52`) and never computes or reports an AFC number.
- `tuner->signal` is scaled linearly across the register's full range. The
  downstream driver hands the signed value from `mt6628_GetCurRSSI()` to
  userspace unchanged (`aquaris-5/mediatek/platform/mt6589/external/meta/fm/
  meta_fm.c:1000-1009`), so there is no vendor 0..65535 mapping to reproduce
  and no calibration constant that would justify converting to dBuV. This is a
  judgement call, made where it is used, and it is the weakest number the
  driver reports.
- `VIDIOC_S_HW_FREQ_SEEK` is not implemented for `O_NONBLOCK`. The underlying
  command is a blocking STP round trip.

### RDS

RDS groups are pushed to the host unsolicited as their own
`RDS_RX_DATA_OPCODE` (0x0d) event packet
(`core/inc/fm_link.h:35`, handled at `core/fm_link.c:389-409`), carrying up to
`MAX_RDS_RX_GROUP_CNT` 12-byte records (`inc/fm_rds.h:15-32`). The
register-level reader the vendor provides, `mt6628_RDS_GetData()` at
`mt6628/pub/mt6628_fm_rds.c:157-219`, is `#if 0`'d out — it is dead code in the
vendor tree as well. There is no "new group available" register to poll in this
path, so the driver decodes in the STP receive workqueue that already exists,
re-checking the power state on every packet because one can still be in flight
when the radio is closed. No second thread was added.

Decoding follows the vendor parser rather than the RDS standard, because this
demodulator does its own error correction and reports only which blocks
survived. A block is usable only when the matching `FM_RDS_GDBK_IND_x` bit is
set (`inc/fm_rds.h:5-8`, checked in `core/fm_rds_parser.c:116-133`), and PS/RT
are reported only once a segment has been received twice identically, the rule
in `rds_g0_ps_cmp()` at `core/fm_rds_parser.c:532-592` and `rds_g2_rt_cmp()` at
`:857`. Every field bit position is cited at the point of use in `mtk-fm.c`.

One defect was found here during the audit and fixed. The radio text was
decoded **back to front**: passing the address of a block to the segment handler
read it in host byte order, so a 2A segment carrying `ABCD` in blocks C and D
published `BADC`. `rds_g2_rt_get()` at `core/fm_rds_parser.c:781-828` writes
each block's high byte first, so the characters are now assembled explicitly in
`mtk_fm_rds_rt_chars()` (`mtk-fm.c:446`).

Two things are deliberately **not** there:

- **Raw block access.** `V4L2_TUNER_CAP_RDS_BLOCK_IO` and the
  `VIDIOC_G_RDS`/`VIDIOC_S_RDS` ioctls are not offered. This tree's
  `include/uapi/linux/videodev2.h` has no such ioctls and no
  `V4L2_EVENT_SUB_CTRL`, so the older draft API that would carry
  `RDS_RADIO_TEXT` and `RDS_DATA_EE_GROUP` on an `EVENT_V4L2_CTRL` cannot be
  expressed without editing the UAPI headers, which are outside this driver's
  scope. The control route above is what this tree's UAPI does support, and it
  is the route the in-tree vivid radio model uses
  (`drivers/media/test-drivers/vivid/vivid-radio-common.c:93-94`).
- **Group types 14, 15 and paging.** The vendor's TMC, EON and paging decoders
  are not ported; only groups 0 (PS/TA/AF) and 2 (radio text) are interpreted.
  The alternative-frequency list is parsed but not published, because this
  tree's UAPI has no control for it.

### Channel quality after a seek

The hardware CQI read is issued after each seek
(`mt6628_cqi_get()` at `mt6628/pub/mt6628_fm_cmd.c:859-876` for the request,
`mt6628_CQI_Get()` at `mt6628_fm_lib.c:897-942` for the answer), and its
result is what `VIDIOC_G_TUNER` reports as `tuner->signal` while it is the
freshest. The record layout and both per-field conversions follow the vendor:
frequency `ch * 10 / 2 + 6400`, RSSI sign-extended from 16 bits then `* 6 / 16`
(`mt6628_fm_lib.c:922-932`). The stereo flag in FM_RSSI_IND is bit 12 —
`FM_BF_STEREO` in `mt6628_GetMonoStereo()` at `mt6628_fm_lib.c:1159-1165` — and
is masked off before the signal conversion, which uses all of bits [9:0].

## FM radio — gaps

The following were worked through one at a time against the vendor source.
Each is either implemented, with the evidence, or documented as unavailable,
with the reason. Nothing is listed here that was neither.

### (a) De-emphasis — implemented, and now read back from the chip

`V4L2_CID_TUNE_DEEMPHASIS` sets FM_MAIN_CG2_CTRL[12] — `DE_EMPHASIS` at
`mt6628/inc/mt6628_fm_reg.h:83`, "0x61 D12, 0:50us, 1:75 us" — through a
read-modify-write of the whole register in `mtk_fm_set_deemphasis()`
(`mtk-fm.c:961`). Only bit 12 is touched, so the antenna type and the
analog/I2S select programmed at power-up are preserved. The default is 50 us,
matching `FM_RX_DEEMPHASIS_MT6628 = 0` at
`mt6628/inc/mt6628_fm_cust_cfg.h:64`.

Two caveats, both of which the audit turned up:

- The vendor writes this bit **only** inside the power-up sequence
  (`mt6628_fm_cmd.c:295`) and has no runtime setter at all. The runtime
  read-modify-write is therefore a small extension of the vendor sequence, not
  a port of one.
- Because that power-up sequence programs 50 us behind the driver's back, the
  control value could disagree with the hardware. The control is therefore
  `V4L2_CTRL_FLAG_VOLATILE` and `mtk_fm_g_volatile_ctrl()` (`mtk-fm.c:2018`)
  reads FM_MAIN_CG2_CTRL[12] back on every `VIDIOC_G_CTRL`. `struct v4l2_tuner`
  has no de-emphasis field, so the control is the only place the state can be
  reported; there is nothing to add to `VIDIOC_G_TUNER`.

`V4L2_CID_AUDIO_MUTE` is volatile for the same reason. `V4L2_CID_AUDIO_VOLUME`
is deliberately **not**: the hardware holds a table value rather than the index,
and mapping it back would mean searching the table for an entry a failed write
may have left absent.

### (b) Mono / stereo — implemented, and made to survive a power cycle

`FM_IOCTL_SETMONOSTERO` reaches `mt6628_SetMonoStereo()`
(`mt6628_fm_lib.c:1176-1192`), which writes 0x3007 to FM_MAIN_CG1_CTRL and then
sets or clears bit 3 of register 0x75 (`FM_FORCE_MS`). Both steps are
reproduced in `mtk_fm_set_force_mono()` (`mtk-fm.c:992`), reached from
`VIDIOC_S_TUNER` with `V4L2_TUNER_MODE_MONO`. Only bit 3 is touched, which is
what the vendor's own read-modify-write helper does.

The audit found this was **not** actually working across a power cycle: 0x75
is one of the registers the power-down sequence resets, so a MONO request
silently reverted to STEREO on the next open. The request is now cached in
`fm->force_mono` and re-applied by `mtk_fm_open()` (`mtk-fm.c:1478`), with mute,
volume and de-emphasis.

Detection was already correct and is unchanged. `tuner->rxsubchans` reports
`V4L2_TUNER_SUB_STEREO` or `_MONO` from FM_RSSI_IND bit 12, per
`mt6628_GetMonoStereo()`. The distinction is worth stating because it is easy
to conflate with `audmode`: `audmode` is what was *asked for* (the force-mono
request), `rxsubchans` is what the demodulator *detected*. A mono-forced tuner
on a stereo station reports `audmode == V4L2_TUNER_MODE_MONO` and
`rxsubchans == V4L2_TUNER_SUB_STEREO`, and that is correct, not a contradiction.

### (c) GETRSSI beyond what CQI provides — nothing further is available

The register is FM_RSSI_IND (0x6c,
`mt6628/inc/mt6628_fm_reg.h:16`). Everything the vendor reads from it is two
things: bits 9:0, the signed RSSI with a 1024 bias and an LSB of 6/16 dB
(`mt6628_GetCurRSSI()`, `mt6628_fm_lib.c:1077-1092`), and bit 12, the stereo
flag (`mt6628_GetMonoStereo()`). Both are used. There is no vendor code that
reads a further field from this register, and the neighbouring FM_RSSI_TH (0x6d)
has no live accessor — see (g). **No further implementation is possible here,
and none is offered.**

`FM_IOCTL_GETCURPAMD` would give a second measurement, from FM_ADDR_PAMD (0xb4),
averaged over eight reads (`mt6628_GetCurPamd()`, `mt6628_fm_lib.c:1209-1248`).
It is not surfaced because `struct v4l2_tuner` has one signal field, not two,
and there is no V4L2 control for a second measurement.

### (d) SCAN — documented as unavailable; it does not fit the V4L2 seek API

`FM_IOCTL_SCAN`, `FM_IOCTL_SCAN_NEW`, `FM_IOCTL_STOP_SCAN`,
`FM_IOCTL_SCAN_GETRSSI` and `FM_IOCTL_PRE_SEARCH`/`FM_IOCTL_RESTORE_SEARCH`
(`core/inc/fm_ioctl.h:17-18`, `:39`, `:58`, `:65-66`) map onto a multi-segment
sweep. Each hardware segment is a `FM_SCAN_OPCODE` command
(`mt6628_scan()`, `mt6628_fm_cmd.c:798-851`) whose answer is a 16-word bitmap
of which channels in the segment carried a signal
(`FM_SCANTBL_SIZE = 16`, `core/inc/fm_link.h:86`, filled at
`core/fm_link.c:358`). The whole band at 50 kHz spacing is 410 channels, so the
vendor walks it in segments of 250 channels (`SCAN_SEG_LEN`,
`mt6628_fm_lib.c:947`) with a cancel check between them
(`mt6628_Scan_50KHz()`, `:950-1039`), and then drains the CQI queue afterwards
for the RSSI of each hit.

**This is not expressible through `VIDIOC_S_HW_FREQ_SEEK`**, and it is worth
being precise about why rather than just asserting it:

- The hardware returns a *set* of frequencies. `VIDIOC_S_HW_FREQ_SEEK` returns
  one, by setting `*p_frequency`, and has no way to hand back a list.
- The bitmap is a firmware-owned format. This tree's `videodev2.h` has no ioctl
  that carries a bitmap or a list of found channels. Inventing one would be
  inventing a private ABI, which is a decision for the operator, not a port.
- Stopping mid-sweep needs a cancel path. The vendor's is a firmware event
  injection, `fm_force_active_event()` from `mt6628_ScanStop()`
  (`mt6628_fm_lib.c:1248-1255`), which has no equivalent here.
- The pre-search and restore-search steps are a ramp-down and mute around the
  sweep (`mt6628_pre_search()`/`:1363-1374`), not separate user-visible states.

What userspace *can* do today is search the band by seeking and reading
`tuner->signal`, which is what `VIDIOC_S_HW_FREQ_SEEK` with
`V4L2_TUNER_CAP_HWSEEK_WRAP` already gives, one channel at a time.

**Recommendation, deliberately not acted on:** if a real scan is wanted, the
honest route is a private MTK ioctl matching `struct fm_scan_t`
(`core/inc/fm_main.h:132-146`), which needs closed vendor userspace to be
useful. It has not been added, because a private ABI with no consumer in this
tree is dead code that looks like a feature.

### (e) DESENSE — the WMT lists are driven; there is no readback to report

`mtk_fm_update_desense()` (`mtk-fm.c:1585`) programs the coex DSNS lists on
every tune, from the same two tables the vendor uses:
`mt6628_mcu_dese_list` (`mt6628_fm_lib.c:1608-1610`) and
`mt6628_gps_dese_list` (`:1612-1614`).

`FM_IOCTL_DESENSE_CHECK` and `FM_IOCTL_IS_DESE_CHAN` read back nothing from the
chip. `mt6628_is_dese_chan()` (`mt6628_fm_lib.c:1658-1675`) searches a third,
*different* table, `mt6628_scan_dese_list` (`:1653-1655`, ten channels), and
`mt6628_desense_check()` (`:1681-1692`) additionally compares the signal level
against a software threshold that only `FM_IOCTL_SET_SEARCH_THRESHOLD` can set —
see (g). Neither value reaches the hardware.

So there is no register to report and no state to expose: **both are pure
userspace arithmetic over a static table in the vendor library, and they are
deliberately not reimplemented here.** The consequence for a user is that the
driver cannot tell applications "this frequency is a known desense channel";
the desense adjustment happens transparently at tune time instead. That is the
better behaviour, since the alternative would push a board-level frequency
table into every application.

### (f) Chip id, hardware version and patch version — not reported anywhere

`FM_IOCTL_GET_HW_INFO` and `FM_IOCTL_GETCHIPID` return `struct fm_hw_info`,
which the vendor fills from a file-static struct (`mt6628_hw_info_get()`,
`mt6628_fm_lib.c:1357-1367`). Its four fields come from four different places,
and it is worth listing them because they are *not* equally available:

| Field | Vendor source | Available here? |
|---|---|---|
| `chip_id` | reg 0x62 vs 0x6628 (`mt6628_fm_lib.c:522`) | yes, at power-up |
| `rom_ver` | 0x83[15:8], `mt6628_fm_lib.c:294` | yes, at power-up |
| `patch_ver` | **coeff blob bytes 38-39** (`:566-568`) | no, firmware file |
| `eco_ver` | `mtk_wcn_wmt_hwver_get()` (`:538`) | no, not in this WMT API |

The driver reads `chip_id` and `rom_ver` at power-up and uses both — the chip id
gates whether the part is an MT6628 at all, and the ROM version selects the
firmware image — but neither is surfaced to userspace. There is no V4L2 field
for it: `struct v4l2_tuner` has no chip-id member, and no standard FM control
carries it. Inventing one would mean a private control with a private ID, which
is the same objection as (d).

`patch_ver` and `eco_ver` are additionally not obtainable without a firmware
blob and a WMT hwver entry that this tree's `include/linux/mfd/mt6628.h` does
not provide, so they could not be reported even if a field existed.

### (g) Search threshold — confirmed dead in the vendor tree

`FM_IOCTL_SET_SEARCH_THRESHOLD` is **not implemented, and cannot be.** The
vendor's only register path is `mt6628_set_RSSITh()`, and it is commented out
in its entirety at `mt6628_fm_lib.c:231-243`, which is why the only call site,
`mt6628_fm_lib.c:688`, is inside dead code as well.

What the ioctl still does downstream is set three software thresholds used only
by `mt6628_soft_mute_tune()` (`mt6628_set_search_th()`, `:1482-1505`), and
`FM_IOCTL_SOFT_MUTE_TUNE` is itself the vendor's experimental soft-mute tuning
probe, which has no V4L2 equivalent and no in-tree consumer. So there is neither
a register to write nor a user of the value. **Documented, not coded.**

### Not implemented on purpose

**`V4L2_CAP_AUDIO` and any PCM path.** As §2.1 records for the
AWB/I2S2/hardware-gain chain, there is no producer for FM audio on this board.
The driver advertises only

```c
fm->vdev.device_caps = V4L2_CAP_RADIO |
                       V4L2_CAP_TUNER |
                       V4L2_CAP_HW_FREQ_SEEK;
```

with no `V4L2_CAP_AUDIO` and no PCM device. `V4L2_CID_AUDIO_MUTE` and
`V4L2_CID_AUDIO_VOLUME` are therefore **tuner-side** controls: the mute gates the
chip's own mute bit in FM_MAIN_CTRL, which is the same bit the downstream
`FM_IOCTL_MUTE` drove (`mt6628_fm_lib.c:213-229`), and the volume writes the
chip's output gain register. They silence and level the chip rather than
attenuating a capture stream, because there is no stream to attenuate.
Adding `V4L2_CAP_AUDIO` or a PCM device would advertise a path that does not
exist.

**The diagnostic ioctls.** `FM_IOCTL_DUMP_REG`, `FM_IOCTL_RW_REG`,
`FM_IOCTL_HOST_RDWR`, `FM_IOCTL_TOP_RDWR`, `FM_IOCTL_EM_TEST`,
`FM_IOCTL_GETBADBNT`, `FM_IOCTL_GETGOODBCNT`, `FM_IOCTL_GETBLERRATIO`,
`FM_IOCTL_GETCURPAMD`, `FM_IOCTL_GET_AUDIO_INFO`, `FM_IOCTL_GET_I2S_INFO` and
`FM_IOCTL_I2S_SETTING` (`core/inc/fm_ioctl.h:18`, `:24-29`, `:37-38`, `:53`,
`:61-62`) are vendor debug and bring-up facilities: a register dumper, raw
register read/write, host and top (chip) register access, an engineering test
hook, and RDS block-error counters. None has a standard V4L2 equivalent, and
porting them would put an unaudited raw register interface on a public device
node. **Not added.**

**The private 58-ioctl MTK ABI as-is.** `core/inc/fm_ioctl.h` declares 58
ioctls, dispatched in `core/fm_module.c`, against a `fm_hw_info`/`fm_scan_t`/
`fm_rssi_req` ABI that only exists in a closed vendor userspace image. Porting
it as-is would be a large amount of untested code with no consumer. It is
**not** ported; where a small piece of it is genuinely useful and has a
standard home — the CQI read, force-mono, volume, de-emphasis, mute — the piece
was taken and the rest left.

### Also still absent, for the record

- **Antenna switch.** `FM_IOCTL_ANA_SWITCH` maps to a real bit —
  `ANTENNA_TYPE`, FM_MAIN_CG2_CTRL[4], 0 for long and 1 for short
  (`mt6628_fm_lib.c:181-210`) — but the driver selects the long antenna
  unconditionally during power-up (`fm_bop_modify(0x61, 0xff63, 0x0000, ...)`,
  matching `mt6628_fm_cmd.c:294`). It is left alone rather than exposed,
  because which antenna this board's FM path actually uses is a board-layout
  fact that is not documented anywhere in either tree, and offering the switch
  could silently select an unconnected antenna. Note that the de-emphasis
  read-modify-write in (a) preserves this bit rather than clobbering it.
- **RDS alternative frequencies.** Parsed, kept, not published; no control for
  it in this tree's UAPI.

## STP transport — what is implemented

Full SDIO framing for function 2, WMT command and event handling, combo patch
download with the multi-patch E1/E2 selection, the coex (desense) path driven
from the FM and WLAN frequency changes, and GPS_SYNC.

## STP transport — gaps

### CRC16 is computed on transmit and verified on receive — unlike the SDIO reference

This driver writes a real CRC16 in the two trailer bytes on TX and checks it
on RX. That is a deliberate divergence from the downstream SDIO path, and it is
worth being precise about what it does and does not buy:

- **The algorithm is the vendor's.** `mt6628_stp_crc16()` in `mtk-stp.c` carries
  the vendor's 256-entry table and loop verbatim from
  `conn_soc/common/linux/pub/osal.c:55-291`. That table is the *reflected*
  polynomial `0xa001` seeded with zero with no final inversion — CRC-16/ARC
  (a.k.a. CRC-16/IBM), **not** CRC-16/CCITT. This was confirmed by
  regenerating the table from the polynomial and comparing all 256 entries;
  `0xa001` reproduces it exactly and `0x8408` does not.
- **The trailer covers the payload only**, matching the downstream BTIF/UART
  write (`stp_core.c:948-951`: `crc = osal_crc16(buffer, length)` then
  low byte, then high byte).
- **Downstream does not do this over SDIO.** Its SDIO branch writes
  `temp[0] = 0x00; temp[1] = 0x00;` (`stp_core.c:869-871`) and the SDIO RX
  parser discards the trailing CRC bytes without reading them
  (`stp_core.c:1873-1882`). Checking `stp_core.c` for `stp_check_crc()`
  inside the SDIO parser branch returns zero hits — the verification at
  `stp_core.c:2362-2380` is reached only from the BTIF/UART parser.
- **So: does the chip validate the CRC over SDIO?** There is no evidence
  either way. The vendor's SDIO driver, which is the shipping configuration
  for this exact transport, neither computes nor checks it, so the MCU cannot
  be shown to be checking it. Computing it on TX is therefore *not* known to
  be necessary — but it is also not known to be harmful, since the trailer is
  two reserved bytes that the SDIO path leaves zero and the MCU demonstrably
  tolerates whatever the host puts there (it is the same two bytes the UART
  path fills with a real CRC).
- **"Verified" on RX therefore means verified by this driver**, not verified by
  the chip. A mismatching frame is dropped and counted in
  `wmt->rx_crc_errors`, with a rate-limited warning naming the task.

The conservative reading is that a zero trailer is what the vendor ships and
what the MCU is known to accept, and that this driver's RX check is stricter
than the reference. The reasons to keep the check anyway:

1. **It is self-consistent by construction.** The checksum written on TX is
   computed by the same function that checks it on RX, so it cannot reject a
   frame that arrived intact.
2. **The cost of not checking is high.** The RX path feeds WMT command
   responses straight into command parsing. A corrupted payload with a wrong
   length or opcode byte is interpreted as a different, valid-looking WMT
   response, which fails silently and far from the cause.
3. **It is observable.** `rx_crc_errors` gives the bring-up a signal that the
   SDIO link is corrupting data, which matters on a bring-up where the link
   has not yet been shown to be reliable.

If hardware testing ever shows the MCU to reject non-zero trailer bytes, the
correct change is to zero the trailer on TX only; the RX check would then
have to go with it, since a firmware that computes nothing must also be one
that expects nothing.

### Sequence numbering and ACK windowing — investigated, and deliberately not ported

STP header byte 0 carries a sequence number in bits 5:3 and an acknowledge in
bits 2:0. This driver always writes the constant `0x80` and never sends an ACK
frame, so those fields are always zero. Downstream, on the BTIF/UART transport,
maintains `txseq`, `txack`, `rxack`, `winspace` and `expected_rxseq`, builds
byte 0 as `0x80 + (txseq << 3) + txack` (`stp_core.c:901`), emits bare 4-byte
ACK frames (`stp_send_ack()`, `stp_core.c:809-857`), NAKs a sequence mismatch and
re-synchronises through `stp_rest_ctx_state()` (`stp_core.c:318-347`).

That machinery was ported as far as the evidence allowed, and the evidence says
**the MT6628 does not use it over SDIO at all.** The sequence of checks:

1. **Downstream's SDIO transmit branch ignores the sequence state.** It writes
   `mtkstp_header[0] = 0x80;` unconditionally and never reads `sequence.txseq`
   or `sequence.txack` (`stp_core.c:869-871`). Only the BTIF/UART branch builds
   the `0x80 + (txseq << 3) + txack` form (`stp_core.c:901`).
2. **Downstream's SDIO receive branch never parses sequence fields.**
   `parser.seq` and `parser.ack` are assigned at exactly one place in the whole
   file — `stp_core.c:2096-2097` — and that line sits inside the
   `btif_fullset_mode` parser branch, which begins at `stp_core.c:2043`. The SDIO
   branch runs from `stp_core.c:1684` to `stp_core.c:2042` and never assigns
   either field.
3. **Consequently the whole acknowledgment machinery is unreachable in SDIO
   mode.** `stp_process_packet()`, `stp_process_rxack()` and `stp_send_ack()`
   are called only from the BTIF/UART parser and from the BTIF/UART send path.
   In SDIO mode nothing ever calls them, so `expected_rxseq`, `winspace`,
   `txseq` and `rxack` are dead state.
4. **This is not an accident of one file.** The second, independent vendor STP
   implementation (`combo/common/core/stp_core.c`) has the same split: its SDIO
   parser branch contains no reference to `parser.seq`, `parser.ack`,
   `stp_process_packet()`, `stp_send_ack()` or `stp_process_rxack()`, while the
   UART branch does. Two trees, same conclusion.

The SDIO `MTKSTP_NAK` state (`stp_core.c:1762-1785`) parses only the type and
length nibbles — it reads no sequence bit either, and it is reached from
`MTKSTP_SYNC` on the SDIO path. `MTKSTP_FW_MSG`, which handles firmware assert
and dump traffic, is likewise SDIO-side and equally sequence-free.

**So the constant `0x80` this driver writes is not a shortcut that loses a
guarantee; it is what the far end is told by the shipping driver on this exact
transport.** Implementing windowing here would mean emitting bare 4-byte ACK
frames and sequence-carrying headers that the MCU demonstrably does not parse,
on a bring-up with no hardware to detect the resulting failure.

**On the post-reset resynchronisation** — the real question in the original
framing of this gap. `stp_rest_ctx_state()` is reached from exactly two places:
the in-band reset path (`mtk_wcn_stp_inband_reset()`, `stp_core.c:3276-3329`,
plus its completion check in the `MTKSTP_FW_MSG` state at `stp_core.c:2399`) and
`mtk_wcn_stp_flush_context()` (`stp_core.c:3402`). Both belong to the BTIF/UART
control model: the in-band reset is a *STP-level* request that the MCU answers
with a `STP_TASK_INDX` frame of `seq == 0 && ack == 0 && length == 0`
(`stp_core.c:2400-2411`). The MT6628 in this tree has no pwrseq GPIO wired up at
all, so the chip is never reset independently of the SoC, and the SDIO endpoint
comes up from a cold boot with both sides at zero. There is no reset event on
this transport for a resynchronisation to be triggered by, and no in-band reset
command ported that could introduce one.

**What was left as a genuine improvement instead.** The one thing the SDIO
parser *should* do, and does not, is resynchronise byte-wise. Downstream's
`MTKSTP_SYNC` state advances one byte at a time and only commits once a full
4-byte header has been seen (`stp_core.c:1690-1757`), so a truncated or
corrupted leading byte costs nothing; this driver's parser walks on the declared
frame length and `break`s out of the loop, discarding the remainder of the
buffer. That is a real difference in robustness, and it was fixed — see the next
paragraph. Everything else in this section is now documented rather than
attempted, deliberately: porting sequence/ACK into a transport whose peer does
not implement it is the plausible-and-wrong option.

### The SDIO receive parser resynchronises one byte at a time

The receive parser used to trust the length field of whatever it found at the
current offset: one bad byte made it walk off the end and throw the rest of the
buffer away, including every valid frame behind it.

It now scans for the `0x80` start marker the way the downstream `MTKSTP_SYNC`
state does (`stp_core.c:1690-1757`), and only commits to a frame once a complete
4-byte header has been seen and its length fits within the bytes remaining. That
is the same rule downstream applies, and it costs one extra comparison per
mis-aligned byte. The downstream parser also accepts `0x55` as a delimiter and
`0x7f` as the start of a resync pattern; neither appears in any MT6628 SDIO
traffic this driver can observe, and the vendor's own SDIO branch discards
alignment padding rather than looking for markers, so only the `0x80` marker is
matched here.

### No PSM, in-band reset or paged dump

Downstream can put the transport into power-save mode, force an in-band reset and
request a paged memory dump for debug. None of that is ported.

### Two of five optional DT properties are set; three are deliberately absent

`mt6628_stp_probe()` picks up these when present. All five are optional, so a
missing one is never a probe failure — it just means the driver uses its default.

| Property | State | Value / why |
|---|---|---|
| `mediatek,coex-ant-mode` | **set** | `<1>`, from the vendor board config |
| `mediatek,sdio-driving-cfg` | **set** | `<0x00077777>`, from the vendor board config |
| `mediatek,co-clock` | absent | vendor board config does not set it; absent means off, which is correct |
| `mediatek,crystal-trim` | absent | per-board calibration, no data exists in this tree |
| `mediatek,fm-strap-mode` | absent | no counterpart in the vendor tree at all |

The two that are set come from the vendor antenna configuration for this combo
chip. `CUSTOM_HAL_ANT=mt6628_ant_m1` is what every MT6628 project config in the
vendor tree selects, and it is also the chip's own default
(`mt6628.defAnt=mt6628_ant_m1.cfg` in `custom/common/hal/ant/WMT.cfg`). The
selected file, `mediatek/custom/common/hal/ant/mt6628_ant_m1/mt6628_ant_m1.cfg`,
contains exactly:

```
coex_wmt_ant_mode=1
wmt_gps_lna_pin=0
wmt_gps_lna_enable=0
sdio_driving_cfg=0x00077777
```

The vendor driver passes those two through unchanged: `coex_wmt_ant_mode` becomes
byte 5 of the WMT coex command (`wmt_ic_6628.c:1323`), and `sdio_driving_cfg` is
split into the DAT0/1, DAT2/3 and CMD drive nibbles
(`wmt_ic_6628.c:1405-1411`). `mtk-stp.c` already does the same thing at
`mt6628_wmt_coex_init()` and `mt6628_wmt_set_sdio_driving()`, so the values arrive
in the chip in the same encoding the vendor uses. Note the *meaning* of the
antenna modes is documented nowhere in the vendor tree — only that every MT6628
board it ships uses 1 — so the value is quoted from the configuration, not
derived.

**One caveat, because these two boards are not the one the vendor config can be
tied to.** The only vendor project config identifiable as a specific board in
this tree is `eastaeon89`, the bq Aquaris 5 — and `mt6589-aquaris5.dts` has no
MT6628 node at all. The node these properties live on is in
`mt6589-lenovo-blade.dtsi`, included only by the Lenovo B6000 and B8000. So for
*these* boards the values rest on "every MT6628 board the vendor ships selects
m1", not on a verified per-board antenna config. That is the strongest evidence
available, and it is why they are set rather than omitted — but if a B6000 or
B8000 antenna layout ever turns out to differ, this property is the thing to
re-check.

**Why the other three stay absent.** Each would be a guess, and a wrong guess is
worse than the default:

- **`mediatek,crystal-trim`** is a per-board calibration value. The vendor reads
  it from NVRAM at offset `0x6D`, where bit 7 says whether the trim is enabled at
  all (`wmt_ic_6628.c:1420-1470`). There is no NVRAM in this tree and no
  calibration data for this board, so there is nothing to quote.
- **`mediatek,co-clock`** is already correct by default. The vendor WMT.cfg for
  this board never sets `co_clock_flag`, and `wmt_lib_co_clock_get()` falls back
  to 0 when the config is absent (`wmt_lib.c:2039-2046`). An absent DT property
  produces exactly the same outcome.
- **`mediatek,fm-strap-mode`** has no vendor counterpart at all — nothing in the
  downstream tree reads a strap mode. The driver's default of 2 is unchanged.



### The optional `gps-sync` pinctrl state does not exist

`gnss-mt6628.c` looks up a `gps-sync` pinctrl state and warns when it is absent.
No device tree in this tree defines such a state, so that branch is dead code on
this board, and it should stay that way.

This was checked rather than assumed, because it is not obvious. The vendor GPS
synchronisation pin is **not** an SoC pinctrl at all. `wmt_func.c:443-458` picks
between the combo chip's own `EEDI` and `EEDO` pins (selected by
`wmt_gps_lna_pin`, which is 0 on this board), and drives it either through the WMT
chip-pin control interface or through `gCmbPinCtrl`, which programs the *combo
chip's* registers with `wmt_core_reg_rw_raw()`. `WMT_IC_PIN_GSYNC` routes to
`mtk_wcn_soc_gps_sync_ctrl()` (`wmt_ic_soc.c:1258-1260`) — a register write, not a
pinmux change.

The real synchronisation path is therefore the WMT register write already
implemented in `mt6628_wmt_gps_sync_ctrl()`, which `mtk_gnss_open()` calls. There
is no SoC pin to name, so there is no property to add.

## GNSS — what is implemented

`gnss-mt6628.c` is a raw byte relay: it registers with the generic GNSS
framework, forwards writes to the MT6628_STP_TASK_GNSS channel and delivers
received data back through the framework's read path.

Note that this is a faithful port, not a shortcut. The downstream kernel side
is *also* only a raw pipe — `stp_chrdev_gps.c` hands bytes to and from the chip
without interpreting them — so there is no STT or MDTS parser to port, in-tree or
downstream. Anything that turns those bytes into NMEA lives above the kernel, in
the vendor userspace, which is out of scope.

## GNSS — gaps

### Suspend releases the GPS function; resume deliberately does not re-enable it

The driver now has a `suspend` callback that releases the GPS function — drops
GPS_SYNC, restores the default pinctrl state and issues the WMT `FUNC_OFF` for
GPS — using the same teardown order as `close()` and `.remove`.

There is intentionally **no** matching resume callback that powers it back on.
That mirrors downstream exactly: its suspend sets `GPS_PWRCTL_OFF`
(`gps/gps.c:369-380`), and its resume deliberately leaves the chip off, with the
comment *"don't power on device automatically"* (`gps/gps.c:382-396`). The GNSS
session belongs to userspace, which reopens the device and reasserts the WMT
`FUNC_ON` itself; doing it from a resume callback would race that open and can
leave the function powered with nobody attached.

The consequence is stated plainly: **a GPS fix in progress does not survive a
system suspend.** That is the upstream behaviour, not an oversight here. A fix
must be restarted by userspace after resume, exactly as it would have to be if
this driver had never had a suspend hook at all.

The generic GNSS framework has no reset or suspend hooks of its own
(`include/linux/gnss.h:29-33` defines only `open`, `close` and `write_raw`), so
this is registered through the platform driver's `dev_pm_ops` rather than
through the framework.

### The downstream GPS ioctls are gone, and the hw version is not as unused as it looks

Downstream exposes `COMBO_IOC_GPS_HWVER`, `COMBO_IOC_RTC_FLAG` and
`COMBO_IOC_CO_CLOCK_FLAG` through a `/dev/mtk_stp_gps` character device
(`conn_soc/common/linux/pub/stp_chrdev_gps.c:36-38`, `:252-268`). None of that exists
here: the generic GNSS framework's `struct gnss_operations`
(`include/linux/gnss.h:29-33`) has only `open`, `close` and `write_raw` and **no
ioctl hook at all**, so there is nowhere to route these without changing the
framework, which is outside this driver's scope.

On the hardware version: `mt6628_wmt_read_versions()` at `mtk-stp.c:798-818` does
read the HW version, and the value is *not* discarded — it is passed straight
into `mt6628_wmt_patch_download(wmt, hw_ver, rom_ver)`, where it selects the E1
versus E2 multi-patch set, and both versions are logged at probe. So the GPS
`GPS_HWVER` ioctl is redundant rather than missing: the same value is already
obtained and used, just not re-exposed to userspace.

## Summary

| Area | State |
|---|---|
| FM tune / seek | complete — bounded and wrapping, 50/100/200 kHz spacing |
| FM mono-stereo | complete; cached across power cycles |
| FM `tuner->signal` | post-seek CQI else FM_RSSI_IND, scaled linearly |
| FM `tuner->afc` | always 0 — no AFC on this chip |
| FM mute | complete; `V4L2_CID_AUDIO_MUTE`, volatile read-back |
| FM volume | complete — `V4L2_CID_AUDIO_VOLUME` → 0x7d gain table |
| FM de-emphasis | complete; volatile read-back |
| FM RDS | groups 0 and 2 only; no raw blocks, TMC/EON/paging |
| FM CQI | complete — read after each seek |
| FM band scan | not exposed; no V4L2 expression, see (d) |
| FM antenna switch | not exposed; long antenna fixed at power-up |
| FM search threshold | not possible; vendor writer is commented out |
| FM chip id / hw version | used internally, not surfaced; no V4L2 field |
| FM audio path | no producer on this board (see §2.1) |
| FM private ioctls | not ported; needs closed vendor userspace |
| STP SDIO framing / WMT / patch download | complete |
| STP coex and GPS_SYNC | complete |
| STP CRC16 | computed on TX, verified on RX — stricter than the downstream SDIO reference |
| STP RX resynchronisation | byte-wise, like the downstream `MTKSTP_SYNC` state |
| STP seq/ack windowing | absent — the chip does not use it on SDIO; see the section for the evidence |
| STP PSM / in-band reset / paged dump | not implemented |
| STP optional DT properties | two set from the vendor antenna config, three deliberately absent |
| Station statistics | signal, tx_packets, tx_retries, tx_failed — nothing more is available |
| GNSS raw relay | complete, and matches downstream (which is also just a pipe) |
| GNSS suspend | releases the GPS function; resume does not re-enable it, as downstream |
| GNSS `gps-sync` pinctrl state | dead code — the pin is inside the combo chip, not an SoC pinctrl |
| GNSS ioctls (`GPS_HWVER` / `RTC_FLAG` / `CO_CLOCK_FLAG`) | absent — no ioctl hook in the generic framework |

Nothing in this section has been run on hardware.
