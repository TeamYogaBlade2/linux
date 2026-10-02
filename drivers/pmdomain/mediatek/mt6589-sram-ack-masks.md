# Note on the MT6589 SPM SRAM acknowledge masks

`mt6589-pm-domains.h` gives every domain a four-bit `sram_pdn_ack_bits`
(`GENMASK(15, 12)`) except MFG and VDE. The stock driver in
`aquaris-5/mediatek/platform/mt6589/kernel/core/mt_spm_mtcmos.c` defines a
different width per domain:

    VDE_SRAM_ACK  (0x1 << 12)
    VEN_SRAM_ACK  (0xf << 12)
    IFR_SRAM_ACK  (0xf << 12)
    ISP_SRAM_ACK  (0x3 << 12)
    DIS_SRAM_ACK  (0xf << 12)
    MFG_SRAM_ACK  (0x1 << 12)

with MD1/MD2 using `MD_SRAM_PDN = (0x1 << 8)` and no acknowledge mask.

## What the data sheet does and does not settle

The data sheet names `SRAM_PDN_ACK` as a single field spanning bits 15:12 in
`ISP_PWR_CON`, `VEN_PWR_CON` and `DIS_PWR_CON`, and identically in
`VDE_PWR_CON` — it draws no distinction between the domains that the stock
driver treats as different widths. It also documents `VDE_PWR_CON` resetting
to `0x0000FF12`, i.e. all four acknowledge bits and all four power-down bits
set.

So where the data sheet and the stock driver disagree, the data sheet does
**not** corroborate the narrower masks; it simply does not address per-domain
width at all. It cannot be used to confirm or refute the stock driver's
per-domain constants, and it is not evidence that ISP is four bits wide.

## How to read the current table

`VDE` uses `BIT(12)`, matching the stock driver. That is a deliberately
*weaker* poll than `GENMASK(15, 12)`: `scpsys_sram_enable()` waits for
`(ctl & sram_pdn_ack_bits) == 0`, so a narrower mask can only make the wait
easier to satisfy, never harder. It therefore cannot introduce a timeout,
and it removes the dependency on three bits that may never clear.

`ISP` keeps `GENMASK(15, 12)`. Narrowing it to the stock driver's
`GENMASK(13, 12)` was tried and reverted: the data sheet names a single
15:12 field there, so the change rested on the stock constant alone against
a conflicting document, for a domain that is powered on while the provider
registers and so never reaches the poll on this configuration.

## Why this matters for the boot failure

`VEN` and `VDE` are the only MT6589 domains carrying
`MTK_SCPD_KEEP_DEFAULT_OFF`, which leaves them powered off at provider
registration so that `genpd_power_on()` actually runs when a consumer
attaches. `DIS` and `ISP` have no such cap and are powered on up front, so
`genpd_power_on()` returns early for them and this code path is never
reached. That asymmetry is why only `larb0` and `larb1` were affected.

Any failure in `scpsys_power_on()` leaves `__genpd_dev_pm_attach()` returning
a bare `-EPROBE_DEFER` with no message, because `dev_pm_domain_attach()` runs
inside `platform_probe()` before `->probe()`. The device then only appears in
the log as

    platform 16010000.larb: deferred probe pending: (reason unknown)

with nothing from the driver's own probe - which is what made this
particularly hard to place.