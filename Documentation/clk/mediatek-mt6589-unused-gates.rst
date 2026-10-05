=================================
MediaTek MT6589 unreferenced clocks
=================================

The MT6589 clock drivers in ``drivers/clk/mediatek/clk-mt6589-*.c`` define
188 clocks.  88 of them are referenced by a Device Tree node; the rest are
either internal graph nodes (which correctly have no consumer) or leaf gates
for blocks this board does not drive.

This file records what the unreferenced leaf gates belong to, so that a later
reader does not mistake them for an oversight and add a consumer for a block
that has no driver.  A wrong clock consumer is worse than a missing one: it
programs a gate register for a block that is not there.

Internal graph nodes (not gates)
================================

Most of the unreferenced clocks are not gates at all but the mux and divider
outputs that form the internal clock graph.  They must not be given DT
consumers; they are reached through the gates below them.

* ``CLK_TOP_MUX_*`` - ``axi_sel``, ``mem_sel``, ``smi_sel``, ``audintbus_sel``,
  ``audio_sel``, ``cam_sel``, ``camtg_sel``, ``dpilvds_sel``, ``fd_sel``,
  ``fix_sel``, ``hyd_sel``, ``irda_sel``, ``jpg_sel``, ``pmicspi_sel``,
  ``uart_sel``, ``usb20_sel``.
* PLL post-dividers - ``CLK_TOP_SYSPLL_D*``, ``CLK_TOP_UNIVPLL_D*``,
  ``CLK_TOP_UNIVPLL1_D*``, ``CLK_TOP_UNIVPLL2_D*``, ``CLK_TOP_MMPLL_D4``,
  ``CLK_TOP_MMPLL_D6``, ``CLK_TOP_LVDSPLL*``, ``CLK_TOP_MEMPLL_MCK_D4``.
* Miscellaneous - ``CLK_TOP_CLK_NULL``, ``CLK_TOP_CLKPH_MCK``,
  ``CLK_TOP_CPUM_TCK_IN``.

Display
=======

Defined in ``clk-mt6589-disp.c``, unreferenced by any node.

* ``CLK_DISP0_SCL`` (``disp0_scl``) - the display scaler.  MT6589 drives its
  panel through the MIPI DSI path, and nothing in this tree uses a scaler
  block.
* ``CLK_DISP0_CMDQ_ENGINE`` / ``CLK_DISP0_CMDQ_SMI`` - the display command
  queue.
* ``CLK_DISP0_GAMMA_ENGINE`` / ``CLK_DISP0_GAMMA_PIXEL`` - the gamma
  correction block.
* ``CLK_DISP0_ROT_ENGINE`` / ``CLK_DISP0_ROT_SMI`` - the hardware rotator.
  The MT6589 has no rotator in use here.
* ``CLK_DISP1_DSI_DIGITAL_LANE`` (``disp1_dsi_digital_lane``) - note this is a
  *different* ID from ``CLK_DISP1_DSI_DIGITAL``, which the ``mt6589-dsi``
  node does reference (mt6589.dtsi).  Reconciling those two is display-driver
  work and out of scope here.
* ``CLK_DISP1_LCD``, ``CLK_DISP1_SLCD``, ``CLK_DISP1_DPI0``, ``CLK_DISP1_DPI1``,
  ``CLK_DISP1_DBI_ENGINE``, ``CLK_DISP1_DBI_OUTPUT``, ``CLK_DISP1_DBI_SMI`` -
  the parallel LCD output paths (SLCD, DPI, DBI).  The Lenovo Blade drives
  MIPI DSI and has no parallel LCD interface wired.

Infrastructure
==============

Defined in ``clk-mt6589-infracfg.c``, unreferenced by any node.

* ``CLK_INFRA_MD1MCUAXI``, ``CLK_INFRA_MD1HWMIXAXI``, ``CLK_INFRA_MD1AHB``,
  ``CLK_INFRA_MD2MCUAXI``, ``CLK_INFRA_MD2HWMIXAXI``, ``CLK_INFRA_MD2AHB`` -
  the two Cortex-M4 MCU subsystems (MD1, MD2).  There is no MCU driver in
  this tree, so no consumer can exist.
* ``CLK_INFRA_CPUM`` (``infra_cpum``) - the M4U-side CPU clock, same reason.
* ``CLK_INFRA_AUDIO``, ``CLK_INFRA_SPI0``, ``CLK_INFRA_DBGCLK``,
  ``CLK_INFRA_SMI``, ``CLK_INFRA_MFGAXI``, ``CLK_INFRA_CCIF0``,
  ``CLK_INFRA_CCIF1``.

.. note::

   The gate array in ``clk-mt6589-infracfg.c`` does not line up with
   ``include/dt-bindings/clock/mediatek,mt6589-clk.h``: ``CLK_INFRA_M4U`` is
   ID 7 in the header but is shifted to 8 in the driver, and the gates run to
   shift 23 while the header stops at 19 plus ``CLK_INFRA_ARMDIV1`` (20).
   Several entries also still carry the original author's ``/* maybe */`` and
   ``/* mt8135 */`` comments.  That misalignment is a separate bug and has
   deliberately not been changed here, because correcting it would renumber
   live gate registers.

Peripheral
==========

Defined in ``clk-mt6589-pericfg.c``, unreferenced by any node.

* ``CLK_PERI0_USB1`` (``peri_usb1``) - this board uses the internal HSIC PHY,
  not the USB1 controller clock path.
* ``CLK_PERI0_APHIF``, ``CLK_PERI0_MDHIF`` - the audio and multimedia PD/HIF
  controllers.  No MT6589 driver in this tree claims them.
* ``CLK_PERI0_IRDA``, ``CLK_PERI0_NLI`` - IrDA and the nativeland interface,
  neither wired on this board.
* ``CLK_PERI1_FHCTL`` (``peri_fhctl``) - the frequency-hopping controller.
  ``clk-fhctl.c`` exists in this tree but no MT6589 node consumes the hop
  clock.

Image
=====

Defined in ``clk-mt6589-img.c``, unreferenced by any node.  The MT6589 camera
capture path has no driver in this tree - the camera dtsi files are GPIO pin
definitions only - so none of these can have a consumer yet.

* ``CLK_IMAGE_CAM_CAM``, ``CLK_IMAGE_CAM_SMI``, ``CLK_IMAGE_COMMON_SMI``,
  ``CLK_IMAGE_SEN_CAM``, ``CLK_IMAGE_SEN_TG``.

GPU
===

* ``CLK_MFG_HYD`` (``mfg_hyd``) - defined in ``clk-mt6589-mfg.c``, unreferenced.
  No GPU driver in this tree drives this clock.

Top-level PMIC SPI
==================

* ``CLK_TOPCK_PMICSPI`` (``topck_pmicspi``) - a top-level PMIC SPI clock,
  distinct from ``CLK_INFRA_PMICSPI``.  The ``pwrap`` node takes the
  infracfg one (mt6589.dtsi).

CPU intermediate clock
======================

``CLK_INFRA_ARMDIV1`` (``armdiv1``) is defined in
``clk-mt6589-infracfg.c`` as a divider on the CPU mux, with
``CLK_SET_RATE_PARENT`` set.  Until the Device Tree was corrected it had no
consumer at all, because the CPU nodes named ``clk26m`` as their
"intermediate" clock.

That is not a cosmetic error.  ``mediatek-cpufreq.c`` reads the clock by that
name and reparents the CPU onto it while ARMPLL is being reprogrammed, then
looks up an OPP matching the intermediate rate to derive a safe intermediate
voltage.  Pointing it at an unrelated always-on oscillator made both the
reparent target and that voltage lookup meaningless.

The CPU nodes now name ``CLK_INFRA_ARMDIV1``, which divides the mux output
through ``TOP_CKDIV1.clkdiv1_sel`` (bits [4:0], datasheet pp. 435-437).

.. warning::

   ``mt6589_armdiv_determine_rate()`` selects the ratio *nearest* the request
   rather than refusing a rate it cannot hit exactly
   (``mt6589_armdiv_find_rate()``, ``clk-mt6589-infracfg.c``).  A mis-specified
   target frequency therefore yields a silently wrong - but valid - rate
   instead of an error.  This is left as-is deliberately: changing it alters
   runtime clock semantics beyond the scope of this correction.  It is called
   out here so the behaviour is not mistaken for exact division.