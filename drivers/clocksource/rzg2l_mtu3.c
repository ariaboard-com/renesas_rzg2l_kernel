// SPDX-License-Identifier: GPL-2.0
/*
 * RZG Timer Support - MTU3
 *
 * Copyright (C) 2021 Renesas Electronics Corp.
 *
 * Based on sh_mtu2.c
 *
 * Copyright (C) 2009 Magnus Damm
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/sh_timer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/clocksource.h>
#include <linux/of_device.h>
#include <linux/reset.h>

struct rzg2l_mtu3_device;

struct rzg2l_mtu3_channel {
	struct rzg2l_mtu3_device *mtu;
	unsigned int index;
	void __iomem *base;
	void __iomem *iostart;
	void __iomem *ioctrl;
	struct clock_event_device ced;
	struct clocksource cs;
	u64 total_cycles;
	raw_spinlock_t lock;
	unsigned long flags;
	bool cs_enabled;
};

struct rzg2l_mtu3_device {
	struct platform_device *pdev;
	void __iomem *mapbase;
	struct clk *clk;
	struct reset_control *rstc;
	unsigned long rate;
	raw_spinlock_t lock; /* Protect the shared registers */
	struct rzg2l_mtu3_channel *channels;
	unsigned int num_channels;
	bool has_clockevent;
	bool has_clocksource;
};

#define TSTR -1 /* shared register */
#define TSTRA -2 /* shared register */
#define TSTRB -3 /* shared register */

#define TCR  0 /* channel register */
#define TMDR 1 /* channel register */
#define TIOR 2 /* channel register */
#define TIER 3 /* channel register */
#define TSR  4 /* channel register */
#define TCNT 5 /* channel register */
#define TGR  6 /* channel register */

#define TCR_CCLR_NONE		(0 << 5)
#define TCR_CCLR_TGRA		(1 << 5)
#define TCR_CCLR_TGRB		(2 << 5)
#define TCR_CCLR_SYNC		(3 << 5)
#define TCR_CCLR_TGRC		(5 << 5)
#define TCR_CCLR_TGRD		(6 << 5)
#define TCR_CCLR_MASK		(7 << 5)
#define TCR_CKEG_RISING		(0 << 3)
#define TCR_CKEG_FALLING	(1 << 3)
#define TCR_CKEG_BOTH		(2 << 3)
#define TCR_CKEG_MASK		(3 << 3)
/* Values 4 to 7 are channel-dependent */
#define TCR_TPSC_P1		(0 << 0)
#define TCR_TPSC_P4		(1 << 0)
#define TCR_TPSC_P16		(2 << 0)
#define TCR_TPSC_P64		(3 << 0)
#define TCR_TPSC_CH0_TCLKA	(4 << 0)
#define TCR_TPSC_CH0_TCLKB	(5 << 0)
#define TCR_TPSC_CH0_TCLKC	(6 << 0)
#define TCR_TPSC_CH0_TCLKD	(7 << 0)
#define TCR_TPSC_CH1_TCLKA	(4 << 0)
#define TCR_TPSC_CH1_TCLKB	(5 << 0)
#define TCR_TPSC_CH1_P256	(6 << 0)
#define TCR_TPSC_CH1_TCNT2	(7 << 0)
#define TCR_TPSC_CH2_TCLKA	(4 << 0)
#define TCR_TPSC_CH2_TCLKB	(5 << 0)
#define TCR_TPSC_CH2_TCLKC	(6 << 0)
#define TCR_TPSC_CH2_P1024	(7 << 0)
#define TCR_TPSC_CH34_P256	(4 << 0)
#define TCR_TPSC_CH34_P1024	(5 << 0)
#define TCR_TPSC_CH34_TCLKA	(6 << 0)
#define TCR_TPSC_CH34_TCLKB	(7 << 0)
#define TCR_TPSC_MASK		(7 << 0)

#define TMDR_BFE		(1 << 6)
#define TMDR_BFB		(1 << 5)
#define TMDR_BFA		(1 << 4)
#define TMDR_MD_NORMAL		(0 << 0)
#define TMDR_MD_PWM_1		(2 << 0)
#define TMDR_MD_PWM_2		(3 << 0)
#define TMDR_MD_PHASE_1		(4 << 0)
#define TMDR_MD_PHASE_2		(5 << 0)
#define TMDR_MD_PHASE_3		(6 << 0)
#define TMDR_MD_PHASE_4		(7 << 0)
#define TMDR_MD_PWM_SYNC	(8 << 0)
#define TMDR_MD_PWM_COMP_CREST	(13 << 0)
#define TMDR_MD_PWM_COMP_TROUGH	(14 << 0)
#define TMDR_MD_PWM_COMP_BOTH	(15 << 0)
#define TMDR_MD_MASK		(15 << 0)

#define TIOC_IOCH(n)		((n) << 4)
#define TIOC_IOCL(n)		((n) << 0)
#define TIOR_OC_RETAIN		(0 << 0)
#define TIOR_OC_0_CLEAR		(1 << 0)
#define TIOR_OC_0_SET		(2 << 0)
#define TIOR_OC_0_TOGGLE	(3 << 0)
#define TIOR_OC_1_CLEAR		(5 << 0)
#define TIOR_OC_1_SET		(6 << 0)
#define TIOR_OC_1_TOGGLE	(7 << 0)
#define TIOR_IC_RISING		(8 << 0)
#define TIOR_IC_FALLING		(9 << 0)
#define TIOR_IC_BOTH		(10 << 0)
#define TIOR_IC_TCNT		(12 << 0)
#define TIOR_MASK		(15 << 0)
#define TIER_TTGE		(1 << 7)
#define TIER_TTGE2		(1 << 6)
#define TIER_TCIEU		(1 << 5)
#define TIER_TCIEV		(1 << 4)
#define TIER_TGIED		(1 << 3)
#define TIER_TGIEC		(1 << 2)
#define TIER_TGIEB		(1 << 1)
#define TIER_TGIEA		(1 << 0)

#define TSR_TCFD		(1 << 7)
#define TSR_TCFU		(1 << 5)
#define TSR_TCFV		(1 << 4)
#define TSR_TGFD		(1 << 3)
#define TSR_TGFC		(1 << 2)
#define TSR_TGFB		(1 << 1)
#define TSR_TGFA		(1 << 0)
#define TSR_TGIA		(1 << 0)

/* private flags */
#define FLAG_CLOCKEVENT (1 << 0)
#define FLAG_CLOCKSOURCE (1 << 1)
#define FLAG_REPROGRAM (1 << 2)
#define FLAG_SKIPEVENT (1 << 3)
#define FLAG_IRQCONTEXT (1 << 4)

static unsigned long mtu3_reg_offs[] = {
	[TCR] = 0,
	[TMDR] = 1,
	[TIOR] = 2,
	[TIER] = 4,
	[TSR] = 5,
	[TCNT] = 6,
	[TGR] = 8,
};

static inline unsigned long rzg2l_mtu3_read(struct rzg2l_mtu3_channel *ch,
					    int reg_nr)
{
	unsigned long offs;

	if (reg_nr == TSTR)
		return ioread8(ch->mtu->mapbase + 0xAB4);
	else if (reg_nr == TSTRA)
		return ioread8(ch->mtu->mapbase + 0x80);
	else if (reg_nr == TSTRB)
		return ioread8(ch->mtu->mapbase + 0x880);
	offs = mtu3_reg_offs[reg_nr];

	if ((reg_nr == TCNT) || (reg_nr == TGR))
		return ioread16(ch->base + offs);
	else
		return ioread8(ch->base + offs);
}

static inline void rzg2l_mtu3_write(struct rzg2l_mtu3_channel *ch, int reg_nr,
				unsigned long value)
{
	unsigned long offs;

	if (reg_nr == TSTR)
		return iowrite8(value, ch->mtu->mapbase + 0xAB4);

	if (reg_nr == TSTRA)
		return iowrite8(value, ch->mtu->mapbase + 0x80);

	if (reg_nr == TSTRB)
		return iowrite8(value, ch->mtu->mapbase + 0x880);

	offs = mtu3_reg_offs[reg_nr];

	if ((reg_nr == TCNT) || (reg_nr == TGR))
		iowrite16(value, ch->base + offs);
	else
		iowrite8(value, ch->base + offs);
}

static void rzg2l_mtu3_start_stop_ch(struct rzg2l_mtu3_channel *ch, int start)
{
	unsigned long flags, value;

	/* start stop register shared by multiple timer channels */
	raw_spin_lock_irqsave(&ch->mtu->lock, flags);
	value = rzg2l_mtu3_read(ch, TSTRA);

	if (start)
		value |= 1 << ch->index;
	else
		value &= ~(1 << ch->index);

	rzg2l_mtu3_write(ch, TSTRA, value);
	value = rzg2l_mtu3_read(ch, TSTRA);
	raw_spin_unlock_irqrestore(&ch->mtu->lock, flags);
}

static int rzg2l_mtu3_enable(struct rzg2l_mtu3_channel *ch)
{
	unsigned long periodic;
	unsigned long rate;
	int ret;

	pm_runtime_get_sync(&ch->mtu->pdev->dev);
	dev_pm_syscore_device(&ch->mtu->pdev->dev, true);

	/* enable clock */
	ret = clk_enable(ch->mtu->clk);
	if (ret) {
		dev_err(&ch->mtu->pdev->dev, "ch%u: cannot enable clock\n",
			ch->index);
		return ret;
	}

	/* make sure channel is disabled */
	rzg2l_mtu3_start_stop_ch(ch, 0);

	rate = clk_get_rate(ch->mtu->clk) / 64;
	periodic = (rate + HZ/2) / HZ;

	/*
	 * "Periodic Counter Operation"
	 * Clear on TGRA compare match, divide clock by 64.
	 */
	if (!(ch->index)%2) {
		rzg2l_mtu3_write(ch, TCR, TCR_TPSC_P64);
		rzg2l_mtu3_write(ch, TIER, 0);
	} else {
		rzg2l_mtu3_write(ch, TCR, TCR_CCLR_TGRA | TCR_TPSC_P64);
		rzg2l_mtu3_write(ch, TIOR, TIOC_IOCH(TIOR_OC_0_CLEAR) |
			TIOC_IOCL(TIOR_OC_0_CLEAR));
		rzg2l_mtu3_write(ch, TGR, periodic);
		rzg2l_mtu3_write(ch, TMDR, TMDR_MD_NORMAL);
		rzg2l_mtu3_write(ch, TIER, TIER_TGIEA);
	}

	/* enable channel */
	rzg2l_mtu3_start_stop_ch(ch, 1);
	return 0;
}

static void rzg2l_mtu3_disable(struct rzg2l_mtu3_channel *ch)
{
	/* disable channel */
	rzg2l_mtu3_start_stop_ch(ch, 0);
	/* stop clock */
	clk_disable(ch->mtu->clk);
	dev_pm_syscore_device(&ch->mtu->pdev->dev, false);
	pm_runtime_put(&ch->mtu->pdev->dev);
}

static int rzg2l_mtu3_start(struct rzg2l_mtu3_channel *ch, unsigned long flag)
{
	int ret = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&ch->lock, flags);

	if (!(ch->flags & (FLAG_CLOCKEVENT | FLAG_CLOCKSOURCE)))
		ret = rzg2l_mtu3_enable(ch);

	if (ret)
		goto out;
	ch->flags |= flag;

	/* setup timeout if no clockevent */
out:
	raw_spin_unlock_irqrestore(&ch->lock, flags);
	return ret;
}

static irqreturn_t rzg2l_mtu3_interrupt(int irq, void *dev_id)
{
	struct rzg2l_mtu3_channel *ch = dev_id;

	/* acknowledge interrupt */
	/* notify clockevent layer */
	if (ch->flags & FLAG_CLOCKEVENT)
		ch->ced.event_handler(&ch->ced);
	return IRQ_HANDLED;
}

static void rzg2l_mtu3_stop(struct rzg2l_mtu3_channel *ch, unsigned long flag)
{
	unsigned long flags;
	unsigned long f;

	raw_spin_lock_irqsave(&ch->lock, flags);
	f = ch->flags & (FLAG_CLOCKEVENT | FLAG_CLOCKSOURCE);
	ch->flags &= ~flag;

	if (f && !(ch->flags & (FLAG_CLOCKEVENT | FLAG_CLOCKSOURCE)))
		rzg2l_mtu3_disable(ch);

	/* adjust the timeout to maximum if only clocksource left */

	raw_spin_unlock_irqrestore(&ch->lock, flags);
}

static struct rzg2l_mtu3_channel *ced_to_rzg2l_mtu3(
				struct clock_event_device *ced)
{
	return container_of(ced, struct rzg2l_mtu3_channel, ced);
}

static int rzg2l_mtu3_clock_event_shutdown(struct clock_event_device *ced)
{
	struct rzg2l_mtu3_channel *ch = ced_to_rzg2l_mtu3(ced);

	if (clockevent_state_periodic(ced))
		rzg2l_mtu3_disable(ch);

	return 0;
}

static int rzg2l_mtu3_clock_event_set_periodic(struct clock_event_device *ced)
{
	struct rzg2l_mtu3_channel *ch = ced_to_rzg2l_mtu3(ced);

	if (clockevent_state_periodic(ced))
		rzg2l_mtu3_disable(ch);

	dev_info(&ch->mtu->pdev->dev, "ch%u: used for periodic clock events\n",
		ch->index);
	rzg2l_mtu3_enable(ch);

	return 0;
}

static void rzg2l_mtu3_clock_event_suspend(struct clock_event_device *ced)
{
	pm_genpd_syscore_poweroff(&ced_to_rzg2l_mtu3(ced)->mtu->pdev->dev);
}

static void rzg2l_mtu3_clock_event_resume(struct clock_event_device *ced)
{
	pm_genpd_syscore_poweron(&ced_to_rzg2l_mtu3(ced)->mtu->pdev->dev);
}

static struct rzg2l_mtu3_channel *cs_to_sh_mtu(struct clocksource *cs)
{
	return container_of(cs, struct rzg2l_mtu3_channel, cs);
}

static u32 rzg2l_mtu3_get_counter(struct rzg2l_mtu3_channel *ch)
{
	u32 v2;

	v2 = rzg2l_mtu3_read(ch, TCNT);

	return v2;
}

static u64 rzg2l_mtu3_clocksource_read(struct clocksource *cs)
{
	struct rzg2l_mtu3_channel *ch = cs_to_sh_mtu(cs);
	unsigned long flags;
	u64 value;
	u32 raw;

	raw_spin_lock_irqsave(&ch->lock, flags);
	value = ch->total_cycles;
	raw = rzg2l_mtu3_get_counter(ch);
	raw_spin_unlock_irqrestore(&ch->lock, flags);

	return value + raw;
}

static int rzg2l_mtu3_clocksource_enable(struct clocksource *cs)
{
	int ret;
	struct rzg2l_mtu3_channel *ch = cs_to_sh_mtu(cs);

	WARN_ON(ch->cs_enabled);
	ch->total_cycles = 0;
	ret = rzg2l_mtu3_start(ch, FLAG_CLOCKSOURCE);
	if (!ret)
		ch->cs_enabled = true;

	return ret;
}

static void rzg2l_mtu3_clocksource_disable(struct clocksource *cs)
{
	struct rzg2l_mtu3_channel *ch = cs_to_sh_mtu(cs);

	WARN_ON(!ch->cs_enabled);
	rzg2l_mtu3_stop(ch, FLAG_CLOCKSOURCE);
	ch->cs_enabled = false;
}

static void rzg2l_mtu3_clocksource_suspend(struct clocksource *cs)
{
	struct rzg2l_mtu3_channel *ch = cs_to_sh_mtu(cs);

	if (!ch->cs_enabled)
		return;
	rzg2l_mtu3_stop(ch, FLAG_CLOCKSOURCE);
	pm_genpd_syscore_poweroff(&ch->mtu->pdev->dev);
}

static void rzg2l_mtu3_clocksource_resume(struct clocksource *cs)
{
	struct rzg2l_mtu3_channel *ch = cs_to_sh_mtu(cs);

	if (!ch->cs_enabled)
		return;
	pm_genpd_syscore_poweron(&ch->mtu->pdev->dev);
	rzg2l_mtu3_start(ch, FLAG_CLOCKSOURCE);
}

static void rzg2l_mtu3_register_clockevent(struct rzg2l_mtu3_channel *ch,
			const char *name)
{
	struct clock_event_device *ced = &ch->ced;

	ced->name = name;
	ced->features = CLOCK_EVT_FEAT_PERIODIC;
	ced->rating = 200;
	ced->cpumask = cpu_possible_mask;
	ced->set_state_shutdown = rzg2l_mtu3_clock_event_shutdown;
	ced->set_state_periodic = rzg2l_mtu3_clock_event_set_periodic;
	ced->suspend = rzg2l_mtu3_clock_event_suspend;
	ced->resume = rzg2l_mtu3_clock_event_resume;
	dev_info(&ch->mtu->pdev->dev, "ch%u: used for clock events\n",
		ch->index);
	clockevents_register_device(ced);
}

static int rzg2l_mtu3_register_clocksource(struct rzg2l_mtu3_channel *ch,
			const char *name)
{
	struct clocksource *cs = &ch->cs;

	cs->name = name;
	cs->rating = 126;
	cs->read = rzg2l_mtu3_clocksource_read;
	cs->enable = rzg2l_mtu3_clocksource_enable;
	cs->disable = rzg2l_mtu3_clocksource_disable;
	cs->suspend = rzg2l_mtu3_clocksource_suspend;
	cs->resume = rzg2l_mtu3_clocksource_resume;
	cs->mask = 0xffff;
	cs->flags = CLOCK_SOURCE_IS_CONTINUOUS;
	dev_info(&ch->mtu->pdev->dev, "ch%u: used as clock source\n",
		ch->index);
	clocksource_register_hz(cs, ch->mtu->rate);
	return 0;
}

static int rzg2l_mtu3_register(struct rzg2l_mtu3_channel *ch,
			       const char *name, int clockevent)
{
	if (clockevent) {
		ch->mtu->has_clockevent = true;
		rzg2l_mtu3_register_clockevent(ch, name);
	} else {
		ch->mtu->has_clocksource = true;
		rzg2l_mtu3_register_clocksource(ch, name);
	}

	return 0;
}

static int rzg2l_mtu3_setup_channel(struct rzg2l_mtu3_channel *ch,
				    unsigned int index,
				    struct rzg2l_mtu3_device *mtu)
{
	static const unsigned int channel_offsets[] = {
		0x100, 0x180, 0x200, 0x000, 0x001, 0xA80, 0x800, 0x801, 0x400
	};
	char name[6];
	int irq;
	int ret;

	ch->mtu = mtu;

	sprintf(name, "tgi%ua", index);
	irq = platform_get_irq_byname(mtu->pdev, name);
	if (irq < 0) {
		/* Skip channels with no declared interrupt. */
		return 0;
	}

	ret = request_irq(irq, rzg2l_mtu3_interrupt,
			  IRQF_TIMER | IRQF_IRQPOLL | IRQF_NOBALANCING,
			  dev_name(&ch->mtu->pdev->dev), ch);
	if (ret) {
		dev_err(&ch->mtu->pdev->dev, "ch%u: failed to request irq %d\n",
			index, irq);
		return ret;
	}

	ch->base = mtu->mapbase + channel_offsets[index];
	ch->index = index;

	return rzg2l_mtu3_register(ch, dev_name(&mtu->pdev->dev), index%2);
}

static int rzg2l_mtu3_map_memory(struct rzg2l_mtu3_device *mtu)
{
	struct resource *res;

	res = platform_get_resource(mtu->pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&mtu->pdev->dev, "failed to get I/O memory\n");
		return -ENXIO;
	}

	mtu->mapbase = ioremap_nocache(res->start, resource_size(res));
	if (mtu->mapbase == NULL)
		return -ENXIO;

	return 0;
}

static int rzg2l_mtu3_setup(struct rzg2l_mtu3_device *mtu,
			struct platform_device *pdev)
{
	unsigned int i;
	int ret;

	mtu->pdev = pdev;
	raw_spin_lock_init(&mtu->lock);

	mtu->rstc = devm_reset_control_get(&pdev->dev, NULL);

	if (IS_ERR(mtu->rstc))
		dev_warn(&pdev->dev, "failed to get cpg reset\n");
	else
		reset_control_deassert(mtu->rstc);

	/* Get hold of clock. */
	mtu->clk = clk_get(&mtu->pdev->dev, "fck");
	if (IS_ERR(mtu->clk)) {
		dev_err(&mtu->pdev->dev, "cannot get clock\n");
		return PTR_ERR(mtu->clk);
	}

	ret = clk_prepare(mtu->clk);
	if (ret < 0)
		goto err_clk_put;

	ret = clk_enable(mtu->clk);
	if (ret < 0)
		goto err_clk_unprepare;

	mtu->rate = clk_get_rate(mtu->clk)/64;
	clk_disable(mtu->clk);

	/* Map the memory resource. */
	ret = rzg2l_mtu3_map_memory(mtu);
	if (ret < 0) {
		dev_err(&mtu->pdev->dev, "failed to remap I/O memory\n");
		goto err_clk_unprepare;
	}

	/* Allocate and setup the channels. */
	mtu->num_channels = 2;

	mtu->channels = kcalloc(mtu->num_channels, sizeof(*mtu->channels),
		GFP_KERNEL);
	if (mtu->channels == NULL) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	for (i = 0; i < mtu->num_channels; ++i) {
		ret = rzg2l_mtu3_setup_channel(&mtu->channels[i], i, mtu);
		if (ret < 0)
			goto err_unmap;
	}

	platform_set_drvdata(pdev, mtu);

	return 0;

err_unmap:
	kfree(mtu->channels);
	iounmap(mtu->mapbase);
err_clk_unprepare:
	clk_unprepare(mtu->clk);
err_clk_put:
	clk_put(mtu->clk);
	return ret;
}

static int rzg2l_mtu3_probe(struct platform_device *pdev)
{
	struct rzg2l_mtu3_device *mtu = platform_get_drvdata(pdev);
	int ret;

	if (!is_early_platform_device(pdev)) {
		pm_runtime_set_active(&pdev->dev);
		pm_runtime_enable(&pdev->dev);
	}

	if (mtu) {
		dev_info(&pdev->dev, "kept as earlytimer\n");
		goto out;
	}

	mtu = kzalloc(sizeof(*mtu), GFP_KERNEL);
	if (mtu == NULL)
		return -ENOMEM;

	ret = rzg2l_mtu3_setup(mtu, pdev);
	if (ret) {
		kfree(mtu);
		pm_runtime_idle(&pdev->dev);
		return ret;
	}

	if (is_early_platform_device(pdev))
		return 0;

out:
	if (mtu->has_clockevent)
		pm_runtime_irq_safe(&pdev->dev);
	else
		pm_runtime_idle(&pdev->dev);

	return 0;
}

static int rzg2l_mtu3_remove(struct platform_device *pdev)
{
	return -EBUSY; /* cannot unregister clockevent */
}

static const struct platform_device_id rzg2l_mtu3_id_table[] = {

	{ "sh-mtu3", 0 },
	{ },
};
MODULE_DEVICE_TABLE(platform, rzg2l_mtu3_id_table);

static const struct of_device_id rzg2l_mtu3_of_table[] __maybe_unused = {
	{ .compatible = "renesas,rzg2l-mtu3" },
	{ }
};
MODULE_DEVICE_TABLE(of, rzg2l_mtu3_of_table);

static struct platform_driver rzg2l_mtu3_device_driver = {
	.probe		= rzg2l_mtu3_probe,
	.remove		= rzg2l_mtu3_remove,
	.driver		= {
		.name	= "rzg2l_mtu3",
		.of_match_table = of_match_ptr(rzg2l_mtu3_of_table),
	},
	.id_table	= rzg2l_mtu3_id_table,
};

static int __init rzg2l_mtu3_init(void)
{
	return platform_driver_register(&rzg2l_mtu3_device_driver);

}

static void __exit rzg2l_mtu3_exit(void)
{
	platform_driver_unregister(&rzg2l_mtu3_device_driver);
}

early_platform_init("earlytimer", &rzg2l_mtu3_device_driver);
subsys_initcall(rzg2l_mtu3_init);
module_exit(rzg2l_mtu3_exit);

MODULE_DESCRIPTION("RZ/G2L MTU3 Timer Driver");
MODULE_LICENSE("GPL v2");

