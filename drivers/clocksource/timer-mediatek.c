// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Mediatek SoCs General-Purpose Timer handling.
 *
 * Copyright (C) 2014 Matthias Brugger
 *
 * Matthias Brugger <matthias.bgg@gmail.com>
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/bug.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqreturn.h>
#include <linux/sched_clock.h>
#include <linux/slab.h>
#include "timer-of.h"

#define TIMER_CLK_EVT           (1)
#define TIMER_CLK_SRC           (2)

#define TIMER_SYNC_TICKS        (3)

/* gpt */
#define GPT_IRQ_EN_REG          0x00
#define GPT_IRQ_ENABLE(val)     BIT((val) - 1)
#define GPT_IRQ_ACK_REG	        0x08
#define GPT_IRQ_ACK(val)        BIT((val) - 1)
#define GPT_IRQ_STA_REG		0x04

#define GPT_CTRL_REG(val)       (0x10 * (val))
#define GPT_CTRL_OP(val)        (((val) & 0x3) << 4)
#define GPT_CTRL_OP_ONESHOT     (0)
#define GPT_CTRL_OP_REPEAT      (1)
#define GPT_CTRL_OP_FREERUN     (3)
#define GPT_CTRL_CLEAR          (2)
#define GPT_CTRL_ENABLE         (1)
#define GPT_CTRL_DISABLE        (0)

#define GPT_CLK_REG(val)        (0x04 + (0x10 * (val)))
#define GPT_CLK_SRC(val)        (((val) & 0x1) << 4)
#define GPT_CLK_SRC_SYS13M      (0)
#define GPT_CLK_SRC_RTC32K      (1)
#define GPT_CLK_DIV1            (0x0)
#define GPT_CLK_DIV2            (0x1)

#define GPT_CNT_REG(val)        (0x08 + (0x10 * (val)))
#define GPT_CMP_REG(val)        (0x0C + (0x10 * (val)))
#define GPT_CNTH_REG(val)       (0x18 + (0x10 * (val)))

/*
 * MT6589 system counter assist.
 *
 * GPT6 is the source of the ARM Generic Timer counter on this SoC.
 * Its upper word may fail to propagate to the Generic Timer until GPT6
 * is accessed through the APB interface.  The downstream workaround
 * uses GPT3 to access GPT6 at each 32-bit rollover.
 *
 * GPT6 is already running when Linux initializes this driver, so do not
 * reset it.  Instead, align GPT3's first interrupt with the next GPT6
 * rollover, then use a 0xffffffff-count repeating interval.
 */
#define MTK_SYSCNT_GPT			6
#define MTK_SYSCNT_ASSIST_GPT		3

#define MTK_SYSCNT_ASSIST_IRQ		GPT_IRQ_ACK(MTK_SYSCNT_ASSIST_GPT)

static bool mtk_syscnt_assist_enabled;
static bool mtk_syscnt_assist_first;
static u32 mtk_syscnt_loop;

static void mtk_syscnt_assist_prepare(struct timer_of *to)
{
	void __iomem *base = timer_of_base(to);
	u32 syscnt_low;
	u32 syscnt_high;
	u32 first_cmp;

	/* Stop and clear GPT3 before programming it. */
	writel(GPT_CTRL_CLEAR | GPT_CTRL_DISABLE,
	       base + GPT_CTRL_REG(MTK_SYSCNT_ASSIST_GPT));

	writel(GPT_CLK_SRC(GPT_CLK_SRC_SYS13M) | GPT_CLK_DIV1,
	       base + GPT_CLK_REG(MTK_SYSCNT_ASSIST_GPT));

	/*
	 * Reading CNT_L snapshots the corresponding CNT_H value.
	 * Keep this order for a coherent 64-bit read.
	 */
	syscnt_low = readl(base + GPT_CNT_REG(MTK_SYSCNT_GPT));
	syscnt_high = readl(base + GPT_CNTH_REG(MTK_SYSCNT_GPT));

	mtk_syscnt_loop = syscnt_high;
	mtk_syscnt_assist_first = true;

	/*
	 * GPT3 starts at zero.  Schedule its first interrupt after the
	 * remaining GPT6 low-word ticks, so the interrupt occurs at the
	 * next 32-bit rollover.
	 *
	 * If the low word is already 0xffffffff, use one tick rather than
	 * programming a zero compare value.
	 */
	first_cmp = ~syscnt_low;
	if (!first_cmp)
		first_cmp = 1;

	writel(first_cmp, base + GPT_CMP_REG(MTK_SYSCNT_ASSIST_GPT));

	/* Acknowledge any stale GPT3 interrupt before enabling it. */
	writel(MTK_SYSCNT_ASSIST_IRQ, base + GPT_IRQ_ACK_REG);

	/* First event is one-shot; the handler switches to repeat mode. */
	writel(GPT_CTRL_OP(GPT_CTRL_OP_ONESHOT) | GPT_CTRL_DISABLE,
	       base + GPT_CTRL_REG(MTK_SYSCNT_ASSIST_GPT));
}

static void mtk_syscnt_assist_start(struct timer_of *to)
{
	void __iomem *base = timer_of_base(to);

	writel(GPT_CTRL_OP(GPT_CTRL_OP_ONESHOT) | GPT_CTRL_ENABLE,
	       base + GPT_CTRL_REG(MTK_SYSCNT_ASSIST_GPT));
}

#if defined(CONFIG_ARM) && defined(CONFIG_ARM_ARCH_TIMER)
static u32 mtk_syscnt_read_cntpct_high(void)
{
	u32 low, high;

	asm volatile("mrrc p15, 0, %0, %1, c14"
		     : "=r" (low), "=r" (high));

	return high;
}
#endif

static void mtk_syscnt_assist_interrupt(struct timer_of *to)
{
	void __iomem *base = timer_of_base(to);
	u32 assist_cnt;
	u32 syscnt_low;
	u32 syscnt_high;
	u32 cnt = 0;

	assist_cnt = readl(base + GPT_CNT_REG(MTK_SYSCNT_ASSIST_GPT));

	/*
	 * Access GPT6 to make its upper word propagate to the Generic
	 * Timer counter.  Read low before high as required by the GPT6
	 * register interface.
	 */
	syscnt_low = readl(base + GPT_CNT_REG(MTK_SYSCNT_GPT));
	syscnt_high = readl(base + GPT_CNTH_REG(MTK_SYSCNT_GPT));

	/*
	 * This handler is expected to run once per GPT6 low-word rollover.
	 * Match the downstream workaround: wait until the GPT6 upper word
	 * reaches the expected value before returning.
	 */
	mtk_syscnt_loop++;

	while (syscnt_high != mtk_syscnt_loop) {
		cnt++;
		syscnt_high = readl(base + GPT_CNTH_REG(MTK_SYSCNT_GPT));
	}

	/*
	 * The first event was aligned to the current GPT6 rollover.
	 * Subsequent events use the downstream 0xffffffff-count repeat
	 * interval.
	 */
	if (mtk_syscnt_assist_first) {
		mtk_syscnt_assist_first = false;

		writel(0xffffffff,
		       base + GPT_CMP_REG(MTK_SYSCNT_ASSIST_GPT));
		writel(GPT_CTRL_CLEAR |
		       GPT_CTRL_OP(GPT_CTRL_OP_REPEAT) |
		       GPT_CTRL_ENABLE,
		       base + GPT_CTRL_REG(MTK_SYSCNT_ASSIST_GPT));
	}

#if defined(CONFIG_ARM) && defined(CONFIG_ARM_ARCH_TIMER)
	WARN_ON_ONCE(mtk_syscnt_read_cntpct_high() != mtk_syscnt_loop);
#endif

	pr_debug("syscnt assist: GPT3=%08x GPT6=%08x:%08x loop=%08x polls=%u\n",
		 assist_cnt, syscnt_high, syscnt_low, mtk_syscnt_loop, cnt);
}

/* system timer */
#define SYST_BASE               (0x40)

#define SYST_CON                (SYST_BASE + 0x0)
#define SYST_VAL                (SYST_BASE + 0x4)

#define SYST_CON_REG(to)        (timer_of_base(to) + SYST_CON)
#define SYST_VAL_REG(to)        (timer_of_base(to) + SYST_VAL)

/*
 * SYST_CON_EN: Clock enable. Shall be set to
 *   - Start timer countdown.
 *   - Allow timeout ticks being updated.
 *   - Allow changing interrupt status,like clear irq pending.
 *
 * SYST_CON_IRQ_EN: Set to enable interrupt.
 *
 * SYST_CON_IRQ_CLR: Set to clear interrupt.
 */
#define SYST_CON_EN              BIT(0)
#define SYST_CON_IRQ_EN          BIT(1)
#define SYST_CON_IRQ_CLR         BIT(4)

static void __iomem *gpt_sched_reg __read_mostly;

static void mtk_syst_ack_irq(struct timer_of *to)
{
	/* Clear and disable interrupt */
	writel(SYST_CON_EN, SYST_CON_REG(to));
	writel(SYST_CON_IRQ_CLR | SYST_CON_EN, SYST_CON_REG(to));
}

static irqreturn_t mtk_syst_handler(int irq, void *dev_id)
{
	struct clock_event_device *clkevt = dev_id;
	struct timer_of *to = to_timer_of(clkevt);

	mtk_syst_ack_irq(to);
	clkevt->event_handler(clkevt);

	return IRQ_HANDLED;
}

static int mtk_syst_clkevt_next_event(unsigned long ticks,
				      struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	/* Enable clock to allow timeout tick update later */
	writel(SYST_CON_EN, SYST_CON_REG(to));

	/*
	 * Write new timeout ticks. Timer shall start countdown
	 * after timeout ticks are updated.
	 */
	writel(ticks, SYST_VAL_REG(to));

	/* Enable interrupt */
	writel(SYST_CON_EN | SYST_CON_IRQ_EN, SYST_CON_REG(to));

	return 0;
}

static int mtk_syst_clkevt_shutdown(struct clock_event_device *clkevt)
{
	/* Clear any irq */
	mtk_syst_ack_irq(to_timer_of(clkevt));

	/* Disable timer */
	writel(0, SYST_CON_REG(to_timer_of(clkevt)));

	return 0;
}

static int mtk_syst_clkevt_resume(struct clock_event_device *clkevt)
{
	return mtk_syst_clkevt_shutdown(clkevt);
}

static int mtk_syst_clkevt_oneshot(struct clock_event_device *clkevt)
{
	return 0;
}

static u64 notrace mtk_gpt_read_sched_clock(void)
{
	return readl_relaxed(gpt_sched_reg);
}

static void mtk_gpt_clkevt_time_stop(struct timer_of *to, u8 timer)
{
	u32 val;

	val = readl(timer_of_base(to) + GPT_CTRL_REG(timer));
	writel(val & ~GPT_CTRL_ENABLE, timer_of_base(to) +
	       GPT_CTRL_REG(timer));
}

static void mtk_gpt_clkevt_time_setup(struct timer_of *to,
				      unsigned long delay, u8 timer)
{
	writel(delay, timer_of_base(to) + GPT_CMP_REG(timer));
}

static void mtk_gpt_clkevt_time_start(struct timer_of *to,
				      bool periodic, u8 timer)
{
	u32 val;

	/* Acknowledge interrupt */
	writel(GPT_IRQ_ACK(timer), timer_of_base(to) + GPT_IRQ_ACK_REG);

	val = readl(timer_of_base(to) + GPT_CTRL_REG(timer));

	/* Clear 2 bit timer operation mode field */
	val &= ~GPT_CTRL_OP(0x3);

	if (periodic)
		val |= GPT_CTRL_OP(GPT_CTRL_OP_REPEAT);
	else
		val |= GPT_CTRL_OP(GPT_CTRL_OP_ONESHOT);

	writel(val | GPT_CTRL_ENABLE | GPT_CTRL_CLEAR,
	       timer_of_base(to) + GPT_CTRL_REG(timer));
}

static int mtk_gpt_clkevt_shutdown(struct clock_event_device *clk)
{
	mtk_gpt_clkevt_time_stop(to_timer_of(clk), TIMER_CLK_EVT);

	return 0;
}

static int mtk_gpt_clkevt_set_periodic(struct clock_event_device *clk)
{
	struct timer_of *to = to_timer_of(clk);

	mtk_gpt_clkevt_time_stop(to, TIMER_CLK_EVT);
	mtk_gpt_clkevt_time_setup(to, to->of_clk.period, TIMER_CLK_EVT);
	mtk_gpt_clkevt_time_start(to, true, TIMER_CLK_EVT);

	return 0;
}

static int mtk_gpt_clkevt_next_event(unsigned long event,
				     struct clock_event_device *clk)
{
	struct timer_of *to = to_timer_of(clk);

	mtk_gpt_clkevt_time_stop(to, TIMER_CLK_EVT);
	mtk_gpt_clkevt_time_setup(to, event, TIMER_CLK_EVT);
	mtk_gpt_clkevt_time_start(to, false, TIMER_CLK_EVT);

	return 0;
}

static irqreturn_t mtk_gpt_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *clkevt = dev_id;
	struct timer_of *to = to_timer_of(clkevt);
	void __iomem *base = timer_of_base(to);
	u32 sta;
	bool handled = false;

	sta = readl(base + GPT_IRQ_STA_REG);

	if (mtk_syscnt_assist_enabled &&
	    (sta & MTK_SYSCNT_ASSIST_IRQ)) {
		writel(MTK_SYSCNT_ASSIST_IRQ, base + GPT_IRQ_ACK_REG);
		mtk_syscnt_assist_interrupt(to);
		handled = true;
	}

	if (sta & GPT_IRQ_ACK(TIMER_CLK_EVT)) {
		writel(GPT_IRQ_ACK(TIMER_CLK_EVT), base + GPT_IRQ_ACK_REG);
		clkevt->event_handler(clkevt);
		handled = true;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static void
__init mtk_gpt_setup(struct timer_of *to, u8 timer, u8 option)
{
	writel(GPT_CTRL_CLEAR | GPT_CTRL_DISABLE,
	       timer_of_base(to) + GPT_CTRL_REG(timer));

	writel(GPT_CLK_SRC(GPT_CLK_SRC_SYS13M) | GPT_CLK_DIV1,
	       timer_of_base(to) + GPT_CLK_REG(timer));

	writel(0x0, timer_of_base(to) + GPT_CMP_REG(timer));

	writel(GPT_CTRL_OP(option) | GPT_CTRL_ENABLE,
	       timer_of_base(to) + GPT_CTRL_REG(timer));
}

static void mtk_gpt_enable_irqs(struct timer_of *to)
{
	void __iomem *base = timer_of_base(to);
	u32 mask;

	/* Disable all GPT interrupts while rebuilding the mask. */
	writel(0, base + GPT_IRQ_EN_REG);

	/* Clear stale pending interrupts from all GPT channels. */
	writel(0x3f, base + GPT_IRQ_ACK_REG);

	mask = GPT_IRQ_ENABLE(TIMER_CLK_EVT);

	if (mtk_syscnt_assist_enabled)
		mask |= GPT_IRQ_ENABLE(MTK_SYSCNT_ASSIST_GPT);

	writel(mask, base + GPT_IRQ_EN_REG);
}

static void mtk_gpt_resume(struct clock_event_device *clk)
{
	struct timer_of *to = to_timer_of(clk);

	if (mtk_syscnt_assist_enabled)
		mtk_syscnt_assist_prepare(to);

	mtk_gpt_enable_irqs(to);

	if (mtk_syscnt_assist_enabled)
		mtk_syscnt_assist_start(to);
}

static void mtk_gpt_suspend(struct clock_event_device *clk)
{
	struct timer_of *to = to_timer_of(clk);

	/* Disable all interrupts */
	writel(0x0, timer_of_base(to) + GPT_IRQ_EN_REG);

	/*
	 * This is called with interrupts disabled,
	 * so we need to ack any interrupt that is pending
	 * or for example ATF will prevent a suspend from completing.
	 */
	writel(0x3f, timer_of_base(to) + GPT_IRQ_ACK_REG);
}

static struct timer_of to = {
	.flags = TIMER_OF_IRQ | TIMER_OF_BASE | TIMER_OF_CLOCK,

	.clkevt = {
		.name = "mtk-clkevt",
		.rating = 300,
		.cpumask = cpu_possible_mask,
	},

	.of_irq = {
		.flags = IRQF_TIMER | IRQF_IRQPOLL,
	},
};

static int __init mtk_syst_init(struct device_node *node)
{
	int ret;

	to.clkevt.features = CLOCK_EVT_FEAT_DYNIRQ | CLOCK_EVT_FEAT_ONESHOT;
	to.clkevt.set_state_shutdown = mtk_syst_clkevt_shutdown;
	to.clkevt.set_state_oneshot = mtk_syst_clkevt_oneshot;
	to.clkevt.tick_resume = mtk_syst_clkevt_resume;
	to.clkevt.set_next_event = mtk_syst_clkevt_next_event;
	to.of_irq.handler = mtk_syst_handler;
	to.of_irq.flags = IRQF_TIMER | IRQF_IRQPOLL;

	ret = timer_of_init(node, &to);
	if (ret)
		return ret;

	clockevents_config_and_register(&to.clkevt, timer_of_rate(&to),
					TIMER_SYNC_TICKS, 0xffffffff);

	return 0;
}

static int __init mtk_gpt_init(struct device_node *node)
{
	int ret;

	mtk_syscnt_assist_enabled =
		of_device_is_compatible(node, "mediatek,mt6589-timer");

	to.clkevt.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	to.clkevt.set_state_shutdown = mtk_gpt_clkevt_shutdown;
	to.clkevt.set_state_periodic = mtk_gpt_clkevt_set_periodic;
	to.clkevt.set_state_oneshot = mtk_gpt_clkevt_shutdown;
	to.clkevt.tick_resume = mtk_gpt_clkevt_shutdown;
	to.clkevt.set_next_event = mtk_gpt_clkevt_next_event;
	to.clkevt.suspend = mtk_gpt_suspend;
	to.clkevt.resume = mtk_gpt_resume;
	to.of_irq.handler = mtk_gpt_interrupt;

	/*
	 * Keep the parent IRQ disabled until GPT1 and the optional GPT3
	 * assist timer have been configured and their pending IRQs cleared.
	 */
	to.of_irq.flags = IRQF_TIMER | IRQF_IRQPOLL | IRQF_NO_AUTOEN;

	ret = timer_of_init(node, &to);
	if (ret)
		return ret;

	/* Configure clock source. */
	mtk_gpt_setup(&to, TIMER_CLK_SRC, GPT_CTRL_OP_FREERUN);
	clocksource_mmio_init(timer_of_base(&to) + GPT_CNT_REG(TIMER_CLK_SRC),
			      node->name, timer_of_rate(&to), 300, 32,
			      clocksource_mmio_readl_up);
	gpt_sched_reg = timer_of_base(&to) + GPT_CNT_REG(TIMER_CLK_SRC);
	sched_clock_register(mtk_gpt_read_sched_clock, 32,
			     timer_of_rate(&to));

	/* Configure clock event. */
	mtk_gpt_setup(&to, TIMER_CLK_EVT, GPT_CTRL_OP_REPEAT);
	clockevents_config_and_register(&to.clkevt, timer_of_rate(&to),
					TIMER_SYNC_TICKS, 0xffffffff);

	/*
	 * GPT6 is already running.  Prepare GPT3 only after GPT1 setup,
	 * so the initial GPT6 sample is close to the assist timer start.
	 */
	if (mtk_syscnt_assist_enabled)
		mtk_syscnt_assist_prepare(&to);

	/* Enable GPT1 and, on MT6589, GPT3 interrupts. */
	mtk_gpt_enable_irqs(&to);

	if (mtk_syscnt_assist_enabled)
		mtk_syscnt_assist_start(&to);

	/* All timer channels are ready; enable the parent IRQ last. */
	enable_irq(timer_of_irq(&to));

	return 0;
}

TIMER_OF_DECLARE(mtk_mt6589, "mediatek,mt6589-timer", mtk_gpt_init);
TIMER_OF_DECLARE(mtk_mt6577, "mediatek,mt6577-timer", mtk_gpt_init);
TIMER_OF_DECLARE(mtk_mt6765, "mediatek,mt6765-timer", mtk_syst_init);
