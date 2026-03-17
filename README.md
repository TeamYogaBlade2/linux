## side note
you may see empty commits like "PHASE 1: ...something...", you should NOT add these commits yourself UNLESS you're adding
new device. these act like a separator for DT changes. phase 2 MUST be sent to upstream after phase 1 is merged, and so on.
driver and dt-bindings changes may be sent at any time.

## markers
- `🟢 OK` - fully working (maybe (perhaps (apparently (i guess (probably...)))))
- `🟡 partial` - it works until it doesn't. use for WIP stuff
- `🔴 TBD` - other device has this working, easy to port to the other one
- `🔴 DEAD` - none of devices has the feature working
- `N/A` - device doesn't have the feature

there may be some inconsistencies since i tend to forget to update stuff or use this map properly...

## Device status
### Generic components
|                                      | JTY D101                                        | Lenovo A369i                |
|--------------------------------------|-------------------------------------------------|-----------------------------|
| DRM                                  | 🟢 OK                                           | 🟡 partial, panel issues    |
| display brightness: pwm-backlight    | 🟢 OK                                           | 🟢 OK                       |
| flashlight                           | 🟢 OK (mediatek,mt6323-led)                     | N/A                         |
| vol +/- keys: mediatek,mt6779-keypad | 🟢 OK                                           | 🟢 OK                       |
| power key: mediatek,mt6323-keys      | 🟢 OK                                           | 🟢 OK                       |
| haptics: regulator-haptic            | 🟢 OK                                           | 🔴 TBD                      |
| charger                              | 🔴 DEAD                                         | 🔴 DEAD                     |

### Per-device components
|                 | JTY D101 | Lenovo A369i                   |
|-----------------|----------|--------------------------------|
| touchscreen     | 🔴 DEAD  | 🔴 DEAD                        |
| panel           | 🟢 OK    | 🟡 partial, needs improvements |
| accelerometer   | 🔴 DEAD  | 🔴 DEAD                        |
| alsps           | 🔴 DEAD  | 🔴 DEAD                        |

## Platform status
everything marked with 'needs upstreaming' means it's not existent in the upstream

### CPU
| component | driver                             | status    | note                                    |
|-----------|------------------------------------|-----------|-----------------------------------------|
| SMP       | arch/arm/mach-mediatek/platsmp.c   | 🟢 OK     |                                         |
| cpufreq   | drivers/cpufreq/mediatek-cpufreq.c | 🟢 OK     |                                         |
| hotplug   | arch/arm/mach-mediatek/platsmp.c   | 🟢 OK     | needs upstreaming (or PSCI)             |
| cpuidle   |                                    | 🔴 DEAD   | needs new driver/PSCI, wfi may work     |
| PMU       | arm,cortex-a7-pmu                  | 🔴 DEAD   | low priority, should be easy to port    |

### Timer
| component  | driver                | status    | note                      |
|------------|-----------------------|-----------|---------------------------|
| APXGPT     | mediatek,mt6577-timer | 🟢 OK     |                           |
| arch timer | arm,armv7-timer       | 🟢 OK     | needs fix upstreaming     |

### Clocks
all of these need upstreaming

also need to get rid of clk_ignore_unused

| component | driver                     | status     | note                                 |
|-----------|----------------------------|------------|--------------------------------------|
| topckgen  | mediatek,mt6572-topckgen   | 🟢 OK      |                                      |
| infracfg  | mediatek,mt6572-infracfg   | 🟢 OK      |                                      |
| apmixed   | mediatek,mt6572-apmixedsys | 🟢 OK      |                                      |
| mmsys     | mediatek,mt6572-mmsys      | 🟢 OK      |                                      |
| mfgcfg    | mediatek,mt6572-mfgcfg     | 🟢 OK      |                                      |
| audio     |                            | 🟢 OK(?)   | not needed for now, can't test       |

### Pinctrl
needs upstreaming, missing slew rate

### Buses
| component | driver                   | status  | note                 |
|-----------|--------------------------|---------|----------------------|
| UART      | mediatek,mt6577-uart     | 🟢 OK   |                      |
| I2C       | mediatek,mt6572-i2c      | 🟢 OK   | needs upstreaming(1) |
| SPI       |                          | 🔴 DEAD |                      |
| USB       | mediatek,mtk-musb        | 🟢 OK   |                      |
| USB PHY   | mediatek,generic-tphy-v1 | 🟢 OK   |                      |

(1) - i2c fix merged into upstream, actual support not sent yet

### Power
### SoC
| component    | driver                           | status     | note                        |
|--------------|----------------------------------|------------|-----------------------------|
| pwrap        | mediatek,mt6572-pwrap            | 🟢 OK      | sent, should be merged soon |
| power domain | mediatek,mt6572-power-controller | 🟢 OK      | needs upstreaming           |

#### PMIC
| component  | driver                    | status    | note                                     |
|------------|---------------------------|-----------|------------------------------------------|
| regulators | mediatek,mt6323-regulator | 🟢 OK     |                                          |
| efuse      | mediatek,mt6323-efuse     | 🟢 OK     | needs upstreaming                        |
| thermal    | mediatek,mt6323-thermal   | 🟢 OK     | needs upstreaming, also tested on mt8163 |
| ADC        | mediatek,mt6323-auxadc    | 🟢 OK     | merged into upstream                     |
| fuel gauge |                           | 🔴 DEAD   | needs new driver                         |

### Storage
| component | driver                     | status     | note                                 |
|-----------|----------------------------|------------|--------------------------------------|
| eMMC      | mediatek,mt2701-mmc        | 🟢 OK      | no PMT parser yet                    |
| microSD   | mediatek,mt2701-mmc        | 🟢 OK      |                                      |
| NAND      |                            | 🔴 DEAD    | no known device with NAND            | 

### SoC
misc SoC components without category

| component        | driver                  | status     | note                                                     |
|------------------|-------------------------|------------|----------------------------------------------------------|
| interrupt parent | mediatek,mt6577-sysirq  | 🟢 OK      |                                                          |
| reset controller | mediatek,mt6572-wdt     | 🟢 OK      | needs upstreaming                                        |
| efuse            | mediatek,mt6572-efuse   | 🟢 OK      | needs upstreaming                                        |
| ADC              | mediatek,mt8173-auxadc  | 🟢 OK      |                                                          |
| thermal          | mediatek,mt6572-thermal | 🟡 partial | temperature is higher (?) than actual, needs upstreaming |

### Display
| component        | driver                                                        | status     | note                                                                                                                                    |
|------------------|---------------------------------------------------------------|------------|-----------------------------------------------------------------------------------------------------------------------------------------|
| MMSYS            | drivers/soc/mediatek/mtk-mmsys.c: mediatek,mt6572-mmsys       | 🟡 partial | no routing table for MDP                                                                                                                |
| DRM              | drivers/gpu/drm/mediatek/mtk_drm_drm.c: mediatek,mt6572-mmsys | 🟢 OK      |                                                                                                                                         |
| IOMMU            | mediatek,mt6572-m4u                                           | 🟢 OK      | needs upstreaming                                                                                                                       |
| SMI              | mediatek,mt6572-smi-common                                    | 🟢 OK      | needs upstreaming                                                                                                                       |
| LARB             | mediatek,mt6572-smi-larb                                      | 🟢 OK      | needs upstreaming                                                                                                                       |
| overlay          | mediatek,mt6572-disp-ovl                                      | 🟢 OK      | needs upstreaming                                                                                                                       |
| read DMA         | mediatek,mt6572-disp-rdma                                     | 🟢 OK      | needs upstreaming                                                                                                                       |
| write DMA        |                                                               | 🔴 DEAD    | no driver (there actually is, but not merged yet...)                                                                                    |
| BLS              | mediatek,mt2701-disp-pwm                                      | 🟢 OK      |                                                                                                                                         |
| color correction | mediatek,mt2701-disp-color                                    | 🟢 OK      |                                                                                                                                         |
| DSI              | mediatek,mt6572-dsi                                           | 🟡 partial | needs upstreaming                                                                                                                       |
| DSI PHY          | mediatek,mt2701-mipi-tx                                       | 🟢 OK      |                                                                                                                                         |
| DBI              |                                                               | 🔴 DEAD    | no known device with DBI display, can't test/port                                                                                       |
| DPI              |                                                               | 🔴 DEAD    | no known device with DPI display, can't test/port                                                                                       |
| hw mutex         | mediatek,mt6572-disp-mutex                                    | 🟡 partial | needs upstreaming, missing mdp ids                                                                                                      |
| CMDQ             |                                                               | 🔴 DEAD    | used only for MDP and camera. technically should be possible to make it work with disp.. but that needs lots of work i guess...         |
| GPU              | arm,mali-400                                                  | 🟢 OK      |                                                                                                                                         |

## External contributions
this is so i can track who contributed to this kernel fork

- [CustomFirmwareDev](https://github.com/gabin8) - i2c dma fix, pwm fix

## Dead subsystems
there's no upstream support OR they need some effort to make them working

### MDP
needs new driver for mdp2.0. existing drivers assume everything is handled on the coproc (vpu/scp) rather than by CPU

### Camera
non existent in the upstream

### HW video enc/dec
mostly useless i'd say, they're not full hw engines but rather only parts of the process. not worth the effort

### Connectivity
not existent in the upstream

### Audio
needs afe/i2s/etc drivers

### Pericfg
it seems to be clock + reset controller for NAND and USB

### EMI mfd
EMI has performance monitoring + bw limiter

### devapc
bus violation monitor? not sure if we really need it, but should be pretty easy to port

### APARM (?)
downstrema calls it APARM_BASE and maps as infrasys? used for watchpoint and breakpoint
