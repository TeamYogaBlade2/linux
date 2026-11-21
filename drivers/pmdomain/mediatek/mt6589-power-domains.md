# Why only VEN and VDE reach the power-on path on MT6589

The MT6589 `scpsys` domains differ in a way that is invisible in the source and
only shows up in the hardware: **which domains are powered at reset.**

`PWR_STATUS` (0x1000660C) and `PWR_STATUS_S` (0x10006610) both reset to
`0x0007E06F` and name their bits:

    fc0[18] fc1[17] fc2[16] fc3[15] cpusys[14] cpudbg[13]
    vdec[8] venc[7] infrasys[6] isp[5] mfg[4] display[3]
    ddrphy[2] md2[1] md1[0]

Decoded, that reset value says:

| domain | bit | at reset |
|---|---|---|
| display | 3 | **1 - already on** |
| isp | 5 | **1 - already on** |
| infrasys | 6 | 1 - already on |
| venc | 7 | **0 - off** |
| vdec | 8 | **0 - off** |
| mfg | 4 | 0 - off |

So DIS and ISP arrive already powered, while VEN and VDE arrive off and have to
be brought up by the kernel.

That lines up exactly with `MTK_SCPD_KEEP_DEFAULT_OFF`. In
`scpsys_add_one_domain()` a domain carrying that cap is deliberately left off
at provider registration so that `genpd_power_on()` runs when a consumer
attaches; a domain without it is powered on during registration and
`genpd_power_on()` then returns early at `genpd_status_on()`. DIS and ISP carry
no cap, so the power-on sequence is never exercised for them.

Which devices use which domain:

    larb0 0x17001000  VEN(7)   off at reset -> power_on() runs   FAILS
    larb1 0x16010000  VDE(8)   off at reset -> power_on() runs   FAILS
    larb2 0x14010000  DIS(3)   on at reset  -> early return      works
    larb3 0x15001000  ISP(5)   on at reset  -> early return      works
    larb4 0x15002000  ISP(5)   on at reset  -> early return      works

MFG is also off at reset and also carries the cap, but its only consumer is the
GPU node, which does not probe on this board, so MFG never runs the sequence
either. VEN and VDE are the only domains that actually execute
`scpsys_power_on()` on a live consumer here, which is why they are the only two
that can fail.

## Why the failure is invisible

`platform_probe()` calls `dev_pm_domain_attach()` before `->probe()`
(`drivers/base/platform.c:1426` then `:1432`). When `genpd_power_on()` fails,
`__genpd_dev_pm_attach()` returns a bare `-EPROBE_DEFER` and discards the
underlying error (`drivers/pmdomain/core.c:3258`) without logging it. The
device's own driver is therefore never entered and prints nothing at all,
which is why `mtk-smi.c` - which has diagnostics on every error path - is
silent, and why the log shows only

    platform 17010000.larb: deferred probe pending: (reason unknown)

Every other `-EPROBE_DEFER` site in this path logs, so the silence itself is
the diagnostic signal that this is a power-domain failure rather than anything
inside the driver.

## What the data sheet does and does not settle about the SRAM ack widths

The data sheet names `SRAM_PDN_ACK` as a single field spanning bits 15:12 in
`ISP_PWR_CON`, `VEN_PWR_CON`, `DIS_PWR_CON` and `VDE_PWR_CON` alike, and
documents `VEN_PWR_CON` resetting to `0x0000FF12` and `VDE_PWR_CON` to
`0x0000FF12` - i.e. all four power-down and all four acknowledge bits set. So
it draws no distinction between the domains the stock driver treats as
differently sized, and it cannot confirm or refute those per-domain widths.

The stock driver in `aquaris-5/mediatek/platform/mt6589/kernel/core/mt_spm_mtcmos.c`
defines one width each:

    VDE_SRAM_ACK  (0x1 << 12)
    VEN_SRAM_ACK  (0xf << 12)
    IFR_SRAM_ACK  (0xf << 12)
    ISP_SRAM_ACK  (0x3 << 12)
    DIS_SRAM_ACK  (0xf << 12)
    MFG_SRAM_ACK  (0x1 << 12)

`VDE` follows that at `BIT(12)`. Narrowing `ISP` the same way was tried and
reverted: the data sheet names a single 15:12 field there, so the change would
have rested on the stock constant alone. The VDE narrowing is safe in the
direction that matters regardless, because `scpsys_sram_enable()` waits for
`(ctl & sram_pdn_ack_bits) == 0`, so a narrower mask can only make the wait
easier to satisfy, never harder.