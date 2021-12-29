// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/proxy-consumer.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/mfd/syscon.h>

#include "../../regulator/internal.h"
#include "gdsc-debug.h"

/* GDSCR */
#define PWR_ON_MASK		BIT(31)
#define CLK_DIS_WAIT_MASK	(0xF << 12)
#define CLK_DIS_WAIT_SHIFT	(12)
#define RETAIN_FF_ENABLE_MASK	BIT(11)
#define SW_OVERRIDE_MASK	BIT(2)
#define HW_CONTROL_MASK		BIT(1)
#define SW_COLLAPSE_MASK	BIT(0)

/* Domain Address */
#define GMEM_CLAMP_IO_MASK	BIT(0)
#define GMEM_RESET_MASK         BIT(4)

/* SW Reset */
#define BCR_BLK_ARES_BIT	BIT(0)

/* Register Offset */
#define REG_OFFSET		0x0

/* Timeout Delay */
#define TIMEOUT_US		100

struct collapse_vote {
	struct regmap	*regmap;
	u32		vote_bit;
};

struct gdsc {
	struct regulator_dev	*rdev;
	struct regulator_desc	rdesc;
	void __iomem		*gdscr;
	struct regmap           *regmap;
	struct regmap           *domain_addr;
	struct regmap           *hw_ctrl;
	struct regmap           *sw_reset;
	struct regmap           *acd_reset;
	struct regmap           *acd_misc_reset;
	struct collapse_vote	collapse_vote;
	struct clk		**clocks;
	struct reset_control	**reset_clocks;
	bool			toggle_logic;
	bool			retain_ff_enable;
	bool			resets_asserted;
	bool			root_en;
	bool			force_root_en;
	bool			no_status_check_on_disable;
	bool			is_gdsc_enabled;
	bool			is_gdsc_hw_ctrl_mode;
	bool			is_root_clk_voted;
	bool			reset_aon;
	int			clock_count;
	int			reset_count;
	int			root_clk_idx;
	u32			gds_timeout;
	bool			skip_disable_before_enable;
};

enum gdscr_status {
	ENABLED,
	DISABLED,
};

static inline u32 gdsc_mb(struct gdsc *gds)
{
	u32 reg;

	regmap_read(gds->regmap, REG_OFFSET, &reg);
	return reg;
}

static int poll_gdsc_status(struct gdsc *sc, enum gdscr_status status)
{
	struct regmap *regmap;
	int count = sc->gds_timeout;
	u32 val;

	if (sc->hw_ctrl)
		regmap = sc->hw_ctrl;
	else
		regmap = sc->regmap;

	for (; count > 0; count--) {
		regmap_read(regmap, REG_OFFSET, &val);
		val &= PWR_ON_MASK;

		switch (status) {
		case ENABLED:
			if (val)
				return 0;
			break;
		case DISABLED:
			if (!val)
				return 0;
			break;
		}
		/*
		 * There is no guarantee about the delay needed for the enable
		 * bit in the GDSCR to be set or reset after the GDSC state
		 * changes. Hence, keep on checking for a reasonable number
		 * of times until the bit is set with the least possible delay
		 * between successive tries.
		 */
		udelay(1);
	}

	return -ETIMEDOUT;
}

static int gdsc_init_is_enabled(struct gdsc *sc)
{
	struct regmap *regmap;
	uint32_t regval, mask;
	int ret;

	if (!sc->toggle_logic) {
		sc->is_gdsc_enabled = !sc->resets_asserted;
		return 0;
	}

	if (sc->collapse_vote.regmap) {
		regmap = sc->collapse_vote.regmap;
		mask = BIT(sc->collapse_vote.vote_bit);
	} else {
		regmap = sc->regmap;
		mask = SW_COLLAPSE_MASK;
	}

	ret = regmap_read(regmap, REG_OFFSET, &regval);
	if (ret < 0)
		return ret;

	sc->is_gdsc_enabled = !(regval & mask);

	return 0;
}

static int gdsc_is_enabled(struct regulator_dev *rdev)
{
	struct gdsc *sc = rdev_get_drvdata(rdev);

	if (!sc->toggle_logic)
		return !sc->resets_asserted;

	if (sc->skip_disable_before_enable)
		return false;

	return sc->is_gdsc_enabled;
}

static int gdsc_enable(struct regulator_dev *rdev)
{
	struct gdsc *sc = rdev_get_drvdata(rdev);
	uint32_t regval, hw_ctrl_regval = 0x0;
	int i, ret = 0;

	if (sc->skip_disable_before_enable)
		return 0;

	if (sc->root_en || sc->force_root_en) {
		clk_prepare_enable(sc->clocks[sc->root_clk_idx]);
		sc->is_root_clk_voted = true;
	}

	regmap_read(sc->regmap, REG_OFFSET, &regval);
	if (regval & HW_CONTROL_MASK) {
		dev_warn(&rdev->dev, "Invalid enable while %s is under HW control\n",
				sc->rdesc.name);
		return -EBUSY;
	}

	if (sc->toggle_logic) {
		if (sc->sw_reset) {
			regmap_read(sc->sw_reset, REG_OFFSET, &regval);
			regval |= BCR_BLK_ARES_BIT;
			regmap_write(sc->sw_reset, REG_OFFSET, regval);

			if (sc->acd_reset)
				regmap_write(sc->acd_reset, REG_OFFSET, regval);

			if (sc->acd_misc_reset)
				regmap_write(sc->acd_misc_reset, REG_OFFSET, regval);

			/*
			 * BLK_ARES should be kept asserted for 1us before
			 * being de-asserted.
			 */
			gdsc_mb(sc);
			udelay(1);

			regval &= ~BCR_BLK_ARES_BIT;
			regmap_write(sc->sw_reset, REG_OFFSET, regval);

			if (sc->acd_reset)
				regmap_write(sc->acd_reset, REG_OFFSET, regval);

			if (sc->acd_misc_reset)
				regmap_write(sc->acd_misc_reset, REG_OFFSET, regval);

			/* Make sure de-assert goes through before continuing */
			gdsc_mb(sc);
		}

		if (sc->domain_addr) {
			if (sc->reset_aon) {
				regmap_read(sc->domain_addr, REG_OFFSET,
								&regval);
				regval |= GMEM_RESET_MASK;
				regmap_write(sc->domain_addr, REG_OFFSET,
								regval);
				/*
				 * Keep reset asserted for at-least 1us before
				 * continuing.
				 */
				gdsc_mb(sc);
				udelay(1);

				regval &= ~GMEM_RESET_MASK;
				regmap_write(sc->domain_addr, REG_OFFSET,
							regval);
				/*
				 * Make sure GMEM_RESET is de-asserted before
				 * continuing.
				 */
				gdsc_mb(sc);
			}

			regmap_read(sc->domain_addr, REG_OFFSET, &regval);
			regval &= ~GMEM_CLAMP_IO_MASK;
			regmap_write(sc->domain_addr, REG_OFFSET, regval);

			/*
			 * Make sure CLAMP_IO is de-asserted before continuing.
			 */
			gdsc_mb(sc);
		}

		/* Enable gdsc */
		if (sc->collapse_vote.regmap) {
			regmap_update_bits(sc->collapse_vote.regmap, REG_OFFSET,
					   BIT(sc->collapse_vote.vote_bit),
					   ~BIT(sc->collapse_vote.vote_bit));
		} else {
			regmap_read(sc->regmap, REG_OFFSET, &regval);
			regval &= ~SW_COLLAPSE_MASK;
			regmap_write(sc->regmap, REG_OFFSET, regval);
		}

		/* Wait for 8 XO cycles before polling the status bit. */
		gdsc_mb(sc);
		udelay(1);

		ret = poll_gdsc_status(sc, ENABLED);
		if (ret) {
			regmap_read(sc->regmap, REG_OFFSET, &regval);

			if (sc->hw_ctrl) {
				regmap_read(sc->hw_ctrl, REG_OFFSET,
						&hw_ctrl_regval);
				dev_warn(&rdev->dev, "%s state (after %d us timeout): 0x%x, GDS_HW_CTRL: 0x%x. Re-polling.\n",
					sc->rdesc.name, sc->gds_timeout,
					regval, hw_ctrl_regval);

				ret = poll_gdsc_status(sc, ENABLED);
				if (ret) {
					regmap_read(sc->regmap, REG_OFFSET,
								&regval);
					regmap_read(sc->hw_ctrl, REG_OFFSET,
							&hw_ctrl_regval);
					dev_err(&rdev->dev, "%s final state (after additional %d us timeout): 0x%x, GDS_HW_CTRL: 0x%x\n",
						sc->rdesc.name, sc->gds_timeout,
						regval, hw_ctrl_regval);
					return ret;
				}
			} else {
				dev_err(&rdev->dev, "%s enable timed out: 0x%x\n",
					sc->rdesc.name,
					regval);
				udelay(sc->gds_timeout);

				regmap_read(sc->regmap, REG_OFFSET, &regval);
				dev_err(&rdev->dev, "%s final state: 0x%x (%d us after timeout)\n",
					sc->rdesc.name, regval,
					sc->gds_timeout);
				return ret;
			}
		}

		if (sc->retain_ff_enable && !(regval & RETAIN_FF_ENABLE_MASK)) {
			regval |= RETAIN_FF_ENABLE_MASK;
			regmap_write(sc->regmap, REG_OFFSET, regval);
		}
	} else {
		for (i = 0; i < sc->reset_count; i++)
			reset_control_deassert(sc->reset_clocks[i]);
		sc->resets_asserted = false;
	}

	/*
	 * If clocks to this power domain were already on, they will take an
	 * additional 4 clock cycles to re-enable after the rail is enabled.
	 * Delay to account for this. A delay is also needed to ensure clocks
	 * are not enabled within 400ns of enabling power to the memories.
	 */
	udelay(1);

	/* Delay to account for staggered memory powerup. */
	udelay(1);

	if (sc->force_root_en) {
		clk_disable_unprepare(sc->clocks[sc->root_clk_idx]);
		sc->is_root_clk_voted = false;
	}

	sc->is_gdsc_enabled = true;

	return ret;
}

static int gdsc_disable(struct regulator_dev *rdev)
{
	struct gdsc *sc = rdev_get_drvdata(rdev);
	uint32_t regval;
	int i, ret = 0, parent_enabled;

	if (rdev->supply) {
		regulator_lock(rdev->supply->rdev);
		parent_enabled = regulator_is_enabled(rdev->supply);
		if (parent_enabled < 0) {
			ret = parent_enabled;
			dev_err(&rdev->dev, "%s unable to check parent enable state, ret=%d\n",
				sc->rdesc.name, ret);
			goto done;
		}

		if (!parent_enabled) {
			dev_err(&rdev->dev, "%s cannot disable GDSC while parent is disabled\n",
				sc->rdesc.name);
			ret = -EIO;
			goto done;
		}
	}

	if (sc->force_root_en) {
		clk_prepare_enable(sc->clocks[sc->root_clk_idx]);
		sc->is_root_clk_voted = true;
	}

	/* Delay to account for staggered memory powerdown. */
	udelay(1);

	if (sc->toggle_logic) {
		if (sc->sw_reset) {
			if (sc->acd_misc_reset)
				regmap_update_bits(sc->acd_misc_reset, REG_OFFSET,
					BCR_BLK_ARES_BIT, BCR_BLK_ARES_BIT);
		}

		/* Disable gdsc */
		if (sc->collapse_vote.regmap) {
			regmap_update_bits(sc->collapse_vote.regmap, REG_OFFSET,
					   BIT(sc->collapse_vote.vote_bit),
					   BIT(sc->collapse_vote.vote_bit));
		} else {
			regmap_read(sc->regmap, REG_OFFSET, &regval);
			regval |= SW_COLLAPSE_MASK;
			regmap_write(sc->regmap, REG_OFFSET, regval);
		}

		/* Wait for 8 XO cycles before polling the status bit. */
		gdsc_mb(sc);
		udelay(1);

		if (sc->no_status_check_on_disable) {
			/*
			 * Add a short delay here to ensure that gdsc_enable
			 * right after it was disabled does not put it in a
			 * weird state.
			 */
			udelay(TIMEOUT_US);
		} else {
			ret = poll_gdsc_status(sc, DISABLED);
			if (ret) {
				regmap_read(sc->regmap, REG_OFFSET, &regval);
				dev_err(&rdev->dev, "%s disable timed out: 0x%x\n",
					sc->rdesc.name, regval);
			}
		}

		if (sc->domain_addr) {
			regmap_read(sc->domain_addr, REG_OFFSET, &regval);
			regval |= GMEM_CLAMP_IO_MASK;
			regmap_write(sc->domain_addr, REG_OFFSET, regval);
		}

	} else {
		for (i = sc->reset_count - 1; i >= 0; i--)
			reset_control_assert(sc->reset_clocks[i]);
		sc->resets_asserted = true;
	}

	/*
	 * Check if gdsc_enable was called for this GDSC. If not, the root
	 * clock will not have been enabled prior to this.
	 */
	if ((sc->is_root_clk_voted && sc->root_en) || sc->force_root_en) {
		clk_disable_unprepare(sc->clocks[sc->root_clk_idx]);
		sc->is_root_clk_voted = false;
	}

	sc->is_gdsc_enabled = false;

done:
	if (rdev->supply)
		regulator_unlock(rdev->supply->rdev);

	return ret;
}

static int gdsc_init_hw_ctrl_mode(struct gdsc *sc)
{
	uint32_t regval;
	int ret;

	ret = regmap_read(sc->regmap, REG_OFFSET, &regval);
	if (ret < 0)
		return ret;

	sc->is_gdsc_hw_ctrl_mode = regval & HW_CONTROL_MASK;

	return 0;
}

static unsigned int gdsc_get_mode(struct regulator_dev *rdev)
{
	struct gdsc *sc = rdev_get_drvdata(rdev);

	return sc->is_gdsc_hw_ctrl_mode ? REGULATOR_MODE_FAST
					: REGULATOR_MODE_NORMAL;
}

static int gdsc_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct gdsc *sc = rdev_get_drvdata(rdev);
	uint32_t regval;
	int ret = 0;

	if (rdev->supply) {
		/*
		 * Ensure that the GDSC parent supply is enabled before
		 * continuing.  This is needed to avoid an unclocked access
		 * of the GDSC control register for GDSCs whose register access
		 * is gated by the parent supply enable state in hardware.
		 * Explicit parent supply locking ensures that the parent enable
		 * state cannot change after checking due to a race with another
		 * consumer.
		 */
		regulator_lock(rdev->supply->rdev);
		ret = regulator_is_enabled(rdev->supply);
		if (ret < 0) {
			dev_err(&rdev->dev, "%s unable to check parent enable state, ret=%d\n",
				sc->rdesc.name, ret);
			goto done;
		} else if (WARN(!ret,
				"%s cannot change GDSC HW/SW control mode while parent is disabled\n",
				sc->rdesc.name)) {
			ret = -EIO;
			goto done;
		}
	}

	ret = regmap_read(sc->regmap, REG_OFFSET, &regval);
	if (ret < 0)
		goto done;

	switch (mode) {
	case REGULATOR_MODE_FAST:
		/* Turn on HW trigger mode */
		regval |= HW_CONTROL_MASK;
		ret = regmap_write(sc->regmap, REG_OFFSET, regval);
		if (ret < 0)
			goto done;
		/*
		 * There may be a race with internal HW trigger signal,
		 * that will result in GDSC going through a power down and
		 * up cycle.  In case HW trigger signal is controlled by
		 * firmware that also poll same status bits as we do, FW
		 * might read an 'on' status before the GDSC can finish
		 * power cycle.  We wait 1us before returning to ensure
		 * FW can't immediately poll the status bit.
		 */
		gdsc_mb(sc);
		udelay(1);
		sc->is_gdsc_hw_ctrl_mode = true;
		break;
	case REGULATOR_MODE_NORMAL:
		/* Turn off HW trigger mode */
		regval &= ~HW_CONTROL_MASK;
		ret = regmap_write(sc->regmap, REG_OFFSET, regval);
		if (ret < 0)
			goto done;
		/*
		 * There may be a race with internal HW trigger signal,
		 * that will result in GDSC going through a power down and
		 * up cycle. Account for this case by waiting 1us before
		 * proceeding.
		 */
		gdsc_mb(sc);
		udelay(1);
		/*
		 * While switching from HW to SW mode, HW may be busy
		 * updating internal required signals. Polling for PWR_ON
		 * ensures that the GDSC switches to SW mode before software
		 * starts to use SW mode.
		 */
		if (sc->is_gdsc_enabled) {
			ret = poll_gdsc_status(sc, ENABLED);
			if (ret) {
				dev_err(&rdev->dev, "%s enable timed out\n",
					sc->rdesc.name);
				goto done;
			}
		}
		sc->is_gdsc_hw_ctrl_mode = false;
		break;
	default:
		ret = -EINVAL;
		break;
	}

done:
	if (rdev->supply)
		regulator_unlock(rdev->supply->rdev);

	return ret;
}

static struct regulator_ops gdsc_ops = {
	.is_enabled = gdsc_is_enabled,
	.enable = gdsc_enable,
	.disable = gdsc_disable,
	.set_mode = gdsc_set_mode,
	.get_mode = gdsc_get_mode,
};

static struct regmap_config gdsc_regmap_config = {
	.reg_bits   = 32,
	.reg_stride = 4,
	.val_bits   = 32,
	.max_register = 0x8,
	.fast_io    = true,
};

void gdsc_debug_print_regs(struct regulator *regulator)
{
	struct gdsc *sc = rdev_get_drvdata(regulator->rdev);
	uint32_t regvals[3] = {0};
	int ret;

	if (!sc) {
		pr_err("Failed to get GDSC Handle\n");
		return;
	}

	ret = regmap_bulk_read(sc->regmap, REG_OFFSET, regvals,
			gdsc_regmap_config.max_register ? 3 : 1);
	if (ret) {
		pr_err("Failed to read %s registers\n", sc->rdesc.name);
		return;
	}

	pr_info("Dumping %s Registers:\n", sc->rdesc.name);
	pr_info("GDSCR: 0x%.8x CFG: 0x%.8x CFG2: 0x%.8x\n",
			regvals[0], regvals[1], regvals[2]);
}
EXPORT_SYMBOL(gdsc_debug_print_regs);

static int gdsc_parse_dt_data(struct gdsc *sc, struct device *dev,
				struct regulator_init_data **init_data)
{
	int ret;

	*init_data = of_get_regulator_init_data(dev, dev->of_node, &sc->rdesc);
	if (*init_data == NULL)
		return -ENOMEM;

	if (of_get_property(dev->of_node, "parent-supply", NULL))
		(*init_data)->supply_regulator = "parent";

	ret = of_property_read_string(dev->of_node, "regulator-name",
					&sc->rdesc.name);
	if (ret)
		return ret;

	if (of_find_property(dev->of_node, "domain-addr", NULL)) {
		sc->domain_addr = syscon_regmap_lookup_by_phandle(dev->of_node,
								"domain-addr");
		if (IS_ERR(sc->domain_addr))
			return PTR_ERR(sc->domain_addr);
	}

	if (of_find_property(dev->of_node, "sw-reset", NULL)) {
		sc->sw_reset = syscon_regmap_lookup_by_phandle(dev->of_node,
								"sw-reset");
		if (IS_ERR(sc->sw_reset))
			return PTR_ERR(sc->sw_reset);
	}

	if (of_find_property(dev->of_node, "acd-reset", NULL)) {
		sc->acd_reset = syscon_regmap_lookup_by_phandle(dev->of_node,
								"acd-reset");
		if (IS_ERR(sc->acd_reset))
			return PTR_ERR(sc->acd_reset);
	}

	if (of_find_property(dev->of_node, "acd-misc-reset", NULL)) {
		sc->acd_misc_reset = syscon_regmap_lookup_by_phandle(dev->of_node,
							"acd-misc-reset");
		if (IS_ERR(sc->acd_misc_reset))
			return PTR_ERR(sc->acd_misc_reset);
	}
	if (of_find_property(dev->of_node, "hw-ctrl-addr", NULL)) {
		sc->hw_ctrl = syscon_regmap_lookup_by_phandle(dev->of_node,
								"hw-ctrl-addr");
		if (IS_ERR(sc->hw_ctrl))
			return PTR_ERR(sc->hw_ctrl);
	}

	sc->gds_timeout = TIMEOUT_US;
	of_property_read_u32(dev->of_node, "qcom,gds-timeout",
				&sc->gds_timeout);

	sc->clock_count = of_property_count_strings(dev->of_node,
							"clock-names");
	if (sc->clock_count == -EINVAL) {
		sc->clock_count = 0;
	} else if (sc->clock_count < 0) {
		dev_err(dev, "Failed to get clock names, ret=%d\n",
			sc->clock_count);
		return sc->clock_count;
	}

	sc->root_en = of_property_read_bool(dev->of_node,
						"qcom,enable-root-clk");
	sc->force_root_en = of_property_read_bool(dev->of_node,
						"qcom,force-enable-root-clk");
	sc->reset_aon = of_property_read_bool(dev->of_node,
						"qcom,reset-aon-logic");
	sc->no_status_check_on_disable = of_property_read_bool(dev->of_node,
					"qcom,no-status-check-on-disable");
	sc->retain_ff_enable = of_property_read_bool(dev->of_node,
						"qcom,retain-regs");
	sc->skip_disable_before_enable = of_property_read_bool(dev->of_node,
					"qcom,skip-disable-before-sw-enable");

	if (of_find_property(dev->of_node, "qcom,collapse-vote", NULL)) {
		ret = of_property_count_u32_elems(dev->of_node,
						  "qcom,collapse-vote");
		if (ret != 2) {
			dev_err(dev, "qcom,collapse-vote needs two values\n");
			return -EINVAL;
		}

		sc->collapse_vote.regmap =
			syscon_regmap_lookup_by_phandle(dev->of_node,
							"qcom,collapse-vote");
		if (IS_ERR(sc->collapse_vote.regmap))
			return PTR_ERR(sc->collapse_vote.regmap);
		ret = of_property_read_u32_index(dev->of_node,
						 "qcom,collapse-vote", 1,
						 &sc->collapse_vote.vote_bit);
		if (ret || sc->collapse_vote.vote_bit > 31) {
			dev_err(dev, "qcom,collapse-vote vote_bit error\n");
			return ret;
		}
	}

	sc->toggle_logic = !of_property_read_bool(dev->of_node,
						"qcom,skip-logic-collapse");
	if (!sc->toggle_logic) {
		sc->reset_count = of_property_count_strings(dev->of_node,
							    "reset-names");
		if (sc->reset_count == -EINVAL) {
			sc->reset_count = 0;
		} else if (sc->reset_count < 0) {
			dev_err(dev, "Failed to get reset clock names\n");
			return sc->reset_count;
		}
	}

	if (of_find_property(dev->of_node, "qcom,support-hw-trigger", NULL)) {
		(*init_data)->constraints.valid_ops_mask |=
				REGULATOR_CHANGE_MODE;
		(*init_data)->constraints.valid_modes_mask |=
				REGULATOR_MODE_NORMAL | REGULATOR_MODE_FAST;
	}

	return 0;
}

static int gdsc_get_resources(struct gdsc *sc, struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret, i;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		dev_err(dev, "Failed to get address resource\n");
		return -EINVAL;
	}

	sc->gdscr = devm_ioremap(dev, res->start, resource_size(res));
	if (sc->gdscr == NULL)
		return -ENOMEM;


	if (of_property_read_bool(dev->of_node, "qcom,no-config-gdscr"))
		gdsc_regmap_config.max_register = 0;

	sc->regmap = devm_regmap_init_mmio(dev, sc->gdscr, &gdsc_regmap_config);
	if (!sc->regmap) {
		dev_err(dev, "Couldn't get regmap\n");
		return -EINVAL;
	}

	sc->clocks = devm_kcalloc(dev, sc->clock_count, sizeof(*sc->clocks),
				  GFP_KERNEL);
	if (sc->clock_count && !sc->clocks)
		return -ENOMEM;

	sc->root_clk_idx = -1;
	for (i = 0; i < sc->clock_count; i++) {
		const char *clock_name;

		of_property_read_string_index(dev->of_node, "clock-names", i,
					      &clock_name);

		sc->clocks[i] = devm_clk_get(dev, clock_name);
		if (IS_ERR(sc->clocks[i])) {
			ret = PTR_ERR(sc->clocks[i]);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "Failed to get %s, ret=%d\n",
					clock_name, ret);
			return ret;
		}

		if (!strcmp(clock_name, "core_root_clk"))
			sc->root_clk_idx = i;
	}

	if ((sc->root_en || sc->force_root_en) && (sc->root_clk_idx == -1)) {
		dev_err(dev, "Failed to get root clock name\n");
		return -EINVAL;
	}

	if (!sc->toggle_logic) {
		sc->reset_clocks = devm_kcalloc(&pdev->dev, sc->reset_count,
						sizeof(*sc->reset_clocks),
						GFP_KERNEL);
		if (sc->reset_count && !sc->reset_clocks)
			return -ENOMEM;

		for (i = 0; i < sc->reset_count; i++) {
			const char *reset_name;

			of_property_read_string_index(pdev->dev.of_node,
						"reset-names", i, &reset_name);
			sc->reset_clocks[i] = devm_reset_control_get(&pdev->dev,
								reset_name);
			if (IS_ERR(sc->reset_clocks[i])) {
				ret = PTR_ERR(sc->reset_clocks[i]);
				if (ret != -EPROBE_DEFER)
					dev_err(&pdev->dev, "Failed to get %s, ret=%d\n",
						reset_name, ret);
				return ret;
			}
		}
	}

	return 0;
}

static int gdsc_probe(struct platform_device *pdev)
{
	static atomic_t gdsc_count = ATOMIC_INIT(-1);
	struct regulator_config reg_config = {};
	struct regulator_init_data *init_data = NULL;
	struct device *dev = &pdev->dev;
	struct gdsc *sc;
	uint32_t regval, clk_dis_wait_val = 0;
	int ret;

	sc = devm_kzalloc(dev, sizeof(*sc), GFP_KERNEL);
	if (sc == NULL)
		return -ENOMEM;

	ret = gdsc_parse_dt_data(sc, dev, &init_data);
	if (ret)
		return ret;

	ret = gdsc_get_resources(sc, pdev);
	if (ret)
		return ret;

	/*
	 * Disable HW trigger: collapse/restore occur based on registers writes.
	 * Disable SW override: Use hardware state-machine for sequencing.
	 */
	regmap_read(sc->regmap, REG_OFFSET, &regval);
	regval &= ~(HW_CONTROL_MASK | SW_OVERRIDE_MASK);

	if (!of_property_read_u32(pdev->dev.of_node, "qcom,clk-dis-wait-val",
				  &clk_dis_wait_val)) {
		clk_dis_wait_val = clk_dis_wait_val << CLK_DIS_WAIT_SHIFT;

		/* Configure wait time between states. */
		regval &= ~(CLK_DIS_WAIT_MASK);
		regval |= clk_dis_wait_val;
	}

	regmap_write(sc->regmap, REG_OFFSET, regval);

	if (!sc->toggle_logic) {
		regval &= ~SW_COLLAPSE_MASK;
		regmap_write(sc->regmap, REG_OFFSET, regval);

		ret = poll_gdsc_status(sc, ENABLED);
		if (ret) {
			dev_err(dev, "%s enable timed out: 0x%x\n",
				sc->rdesc.name, regval);
			return ret;
		}
	}

	ret = gdsc_init_is_enabled(sc);
	if (ret) {
		dev_err(dev, "%s failed to get initial enable state, ret=%d\n",
			sc->rdesc.name, ret);
		return ret;
	}

	ret = gdsc_init_hw_ctrl_mode(sc);
	if (ret) {
		dev_err(dev, "%s failed to get initial hw_ctrl state, ret=%d\n",
			sc->rdesc.name, ret);
		return ret;
	}

	sc->rdesc.id = atomic_inc_return(&gdsc_count);
	sc->rdesc.ops = &gdsc_ops;
	sc->rdesc.type = REGULATOR_VOLTAGE;
	sc->rdesc.owner = THIS_MODULE;

	reg_config.dev = dev;
	reg_config.init_data = init_data;
	reg_config.driver_data = sc;
	reg_config.of_node = dev->of_node;
	reg_config.regmap = sc->regmap;

	sc->rdev = devm_regulator_register(dev, &sc->rdesc, &reg_config);
	if (IS_ERR(sc->rdev)) {
		ret = PTR_ERR(sc->rdev);
		dev_err(dev, "regulator_register(\"%s\") failed, ret=%d\n",
			sc->rdesc.name, ret);
		return ret;
	}

	ret = devm_regulator_proxy_consumer_register(dev, dev->of_node);
	if (ret)
		dev_err(dev, "failed to register proxy consumer, ret=%d\n",
			ret);

	platform_set_drvdata(pdev, sc);

	return ret;
}

static const struct of_device_id gdsc_match_table[] = {
	{ .compatible = "qcom,gdsc" },
	{}
};

static struct platform_driver gdsc_driver = {
	.probe  = gdsc_probe,
	.driver = {
		.name = "gdsc",
		.of_match_table = gdsc_match_table,
		.sync_state = regulator_proxy_consumer_sync_state,
	},
};

static int __init gdsc_init(void)
{
	return platform_driver_register(&gdsc_driver);
}
subsys_initcall(gdsc_init);

static void __exit gdsc_exit(void)
{
	platform_driver_unregister(&gdsc_driver);
}
module_exit(gdsc_exit);
