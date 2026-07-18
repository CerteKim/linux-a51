// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2019, Linaro Limited

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/wcd934x/registers.h>
#include <linux/mfd/wcd934x/wcd934x.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slimbus.h>

#define WCD934X_REGMAP_IRQ_REG(_irq, _off, _mask)		\
	[_irq] = {						\
		.reg_offset = (_off),				\
		.mask = (_mask),				\
		.type = {					\
			.type_reg_offset = (_off),		\
			.types_supported = IRQ_TYPE_EDGE_BOTH,	\
			.type_reg_mask  = (_mask),		\
			.type_level_low_val = (_mask),		\
			.type_level_high_val = (_mask),		\
			.type_falling_val = 0,			\
			.type_rising_val = 0,			\
		},						\
	}

static const struct mfd_cell wcd934x_devices[] = {
	{
		.name = "wcd934x-codec",
	}, {
		.name = "wcd934x-gpio",
		.of_compatible = "qcom,wcd9340-gpio",
	}, {
		.name = "wcd934x-soundwire",
		.of_compatible = "qcom,soundwire-v1.3.0",
	},
};

static const struct regmap_irq wcd934x_irqs[] = {
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_SLIMBUS, 0, BIT(0)),
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_HPH_PA_OCPL_FAULT, 0, BIT(2)),
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_HPH_PA_OCPR_FAULT, 0, BIT(3)),
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_MBHC_SW_DET, 1, BIT(0)),
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_MBHC_ELECT_INS_REM_DET, 1, BIT(1)),
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_MBHC_BUTTON_PRESS_DET, 1, BIT(2)),
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_MBHC_BUTTON_RELEASE_DET, 1, BIT(3)),
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_MBHC_ELECT_INS_REM_LEG_DET, 1, BIT(4)),
	WCD934X_REGMAP_IRQ_REG(WCD934X_IRQ_SOUNDWIRE, 2, BIT(4)),
};

static const unsigned int wcd934x_config_regs[] = {
	WCD934X_INTR_LEVEL0,
};

static const struct regmap_irq_chip wcd934x_regmap_irq_chip = {
	.name = "wcd934x_irq",
	.status_base = WCD934X_INTR_PIN1_STATUS0,
	.mask_base = WCD934X_INTR_PIN1_MASK0,
	.ack_base = WCD934X_INTR_PIN1_CLEAR0,
	.num_regs = 4,
	.irqs = wcd934x_irqs,
	.num_irqs = ARRAY_SIZE(wcd934x_irqs),
	.config_base = wcd934x_config_regs,
	.num_config_bases = ARRAY_SIZE(wcd934x_config_regs),
	.num_config_regs = 4,
	.set_type_config = regmap_irq_set_type_config_simple,
};

static bool wcd934x_is_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case WCD934X_INTR_PIN1_STATUS0...WCD934X_INTR_PIN2_CLEAR3:
	case WCD934X_SWR_AHB_BRIDGE_RD_DATA_0:
	case WCD934X_SWR_AHB_BRIDGE_RD_DATA_1:
	case WCD934X_SWR_AHB_BRIDGE_RD_DATA_2:
	case WCD934X_SWR_AHB_BRIDGE_RD_DATA_3:
	case WCD934X_SWR_AHB_BRIDGE_ACCESS_STATUS:
	case WCD934X_ANA_MBHC_RESULT_3:
	case WCD934X_ANA_MBHC_RESULT_2:
	case WCD934X_ANA_MBHC_RESULT_1:
	case WCD934X_ANA_MBHC_MECH:
	case WCD934X_ANA_MBHC_ELECT:
	case WCD934X_ANA_MBHC_ZDET:
	case WCD934X_ANA_MICB2:
	case WCD934X_ANA_RCO:
	case WCD934X_ANA_BIAS:
		return true;
	default:
		return false;
	}
};

static const struct regmap_range_cfg wcd934x_ranges[] = {
	{	.name = "WCD934X",
		.range_min =  0x0,
		.range_max =  WCD934X_MAX_REGISTER,
		.selector_reg = WCD934X_SEL_REGISTER,
		.selector_mask = WCD934X_SEL_MASK,
		.selector_shift = WCD934X_SEL_SHIFT,
		.window_start = WCD934X_WINDOW_START,
		.window_len = WCD934X_WINDOW_LENGTH,
	},
};

static const struct regmap_config wcd934x_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_MAPLE,
	.max_register = 0xffff,
	.can_multi_write = true,
	.ranges = wcd934x_ranges,
	.num_ranges = ARRAY_SIZE(wcd934x_ranges),
	.volatile_reg = wcd934x_is_volatile_register,
};

struct wcd934x_diag_reg {
	const char *name;
	unsigned int reg;
};

static const struct wcd934x_diag_reg wcd934x_diag_regs[] = {
	{ "chip_id_byte0", WCD934X_CHIP_TIER_CTRL_CHIP_ID_BYTE0 },
	{ "chip_id_byte1", WCD934X_CHIP_TIER_CTRL_CHIP_ID_BYTE0 + 1 },
	{ "chip_id_byte2", WCD934X_CHIP_TIER_CTRL_CHIP_ID_BYTE2 },
	{ "chip_id_byte3", WCD934X_CHIP_TIER_CTRL_CHIP_ID_BYTE2 + 1 },
	{ "intr_status0", WCD934X_INTR_PIN1_STATUS0 },
	{ "intr_status1", WCD934X_INTR_PIN1_STATUS0 + 1 },
	{ "intr_status2", WCD934X_INTR_PIN1_STATUS0 + 2 },
	{ "intr_status3", WCD934X_INTR_PIN1_STATUS0 + 3 },
	{ "intr_mask0", WCD934X_INTR_PIN1_MASK0 },
	{ "intr_mask1", WCD934X_INTR_PIN1_MASK0 + 1 },
	{ "intr_mask2", WCD934X_INTR_PIN1_MASK0 + 2 },
	{ "intr_mask3", WCD934X_INTR_PIN1_MASK0 + 3 },
	{ "intr_level0", WCD934X_INTR_LEVEL0 },
	{ "intr_level1", WCD934X_INTR_LEVEL0 + 1 },
	{ "intr_level2", WCD934X_INTR_LEVEL0 + 2 },
	{ "intr_level3", WCD934X_INTR_LEVEL0 + 3 },
	{ "ana_bias", WCD934X_ANA_BIAS },
	{ "ana_mbhc_mech", WCD934X_ANA_MBHC_MECH },
	{ "ana_mbhc_elect", WCD934X_ANA_MBHC_ELECT },
	{ "ana_mbhc_zdet", WCD934X_ANA_MBHC_ZDET },
	{ "ana_mbhc_result_1", WCD934X_ANA_MBHC_RESULT_1 },
	{ "ana_mbhc_result_2", WCD934X_ANA_MBHC_RESULT_2 },
	{ "ana_mbhc_result_3", WCD934X_ANA_MBHC_RESULT_3 },
	{ "ana_micb2", WCD934X_ANA_MICB2 },
	{ "mbhc_ctl_clk", WCD934X_MBHC_CTL_CLK },
	{ "mbhc_ctl_bcs", WCD934X_MBHC_CTL_BCS },
	{ "mbhc_status_spare_1", WCD934X_MBHC_STATUS_SPARE_1 },
	{ "mbhc_new_ctl_1", WCD934X_MBHC_NEW_CTL_1 },
	{ "mbhc_new_ctl_2", WCD934X_MBHC_NEW_CTL_2 },
	{ "mbhc_new_plug_detect_ctl", WCD934X_MBHC_NEW_PLUG_DETECT_CTL },
	{ "mbhc_new_zdet_ana_ctl", WCD934X_MBHC_NEW_ZDET_ANA_CTL },
	{ "mbhc_new_zdet_ramp_ctl", WCD934X_MBHC_NEW_ZDET_RAMP_CTL },
	{ "mbhc_new_fsm_status", WCD934X_MBHC_NEW_FSM_STATUS },
	{ "mbhc_new_adc_result", WCD934X_MBHC_NEW_ADC_RESULT },
};

struct wcd934x_diag_snapshot {
	int ret[ARRAY_SIZE(wcd934x_diag_regs)];
	unsigned int val[ARRAY_SIZE(wcd934x_diag_regs)];
};

static int wcd934x_diag_read_snapshot(struct wcd934x_ddata *ddata,
				      struct wcd934x_diag_snapshot *snap)
{
	int ret = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(wcd934x_diag_regs); i++) {
		snap->ret[i] = regmap_read(ddata->regmap,
					   wcd934x_diag_regs[i].reg,
					   &snap->val[i]);
		if (snap->ret[i])
			ret = snap->ret[i];
	}

	return ret;
}

static void wcd934x_diag_log_snapshot(struct wcd934x_ddata *ddata,
				      const char *tag, unsigned int elapsed_ms,
				      const struct wcd934x_diag_snapshot *snap)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(wcd934x_diag_regs); i++) {
		if (snap->ret[i]) {
			dev_info(ddata->dev, "passive diag %s %ums %s 0x%04x error=%d\n",
				 tag, elapsed_ms, wcd934x_diag_regs[i].name,
				 wcd934x_diag_regs[i].reg, snap->ret[i]);
			continue;
		}

		dev_info(ddata->dev, "passive diag %s %ums %s 0x%04x=0x%02x\n",
			 tag, elapsed_ms, wcd934x_diag_regs[i].name,
			 wcd934x_diag_regs[i].reg, snap->val[i]);
	}
}

static bool wcd934x_diag_snapshot_changed(struct wcd934x_ddata *ddata,
					  unsigned int elapsed_ms,
					  const struct wcd934x_diag_snapshot *old,
					  const struct wcd934x_diag_snapshot *new)
{
	bool changed = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(wcd934x_diag_regs); i++) {
		if (old->ret[i] == new->ret[i] &&
		    old->val[i] == new->val[i])
			continue;

		changed = true;

		if (new->ret[i]) {
			dev_info(ddata->dev,
				 "passive diag changed %ums %s 0x%04x error=%d\n",
				 elapsed_ms, wcd934x_diag_regs[i].name,
				 wcd934x_diag_regs[i].reg, new->ret[i]);
			continue;
		}

		if (old->ret[i]) {
			dev_info(ddata->dev,
				 "passive diag changed %ums %s 0x%04x error=%d -> 0x%02x\n",
				 elapsed_ms, wcd934x_diag_regs[i].name,
				 wcd934x_diag_regs[i].reg, old->ret[i], new->val[i]);
			continue;
		}

		dev_info(ddata->dev,
			 "passive diag changed %ums %s 0x%04x 0x%02x -> 0x%02x\n",
			 elapsed_ms, wcd934x_diag_regs[i].name,
			 wcd934x_diag_regs[i].reg, old->val[i], new->val[i]);
	}

	return changed;
}

static ssize_t diag_regs_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct wcd934x_ddata *ddata = dev_get_drvdata(dev);
	unsigned int val;
	ssize_t len = 0;
	int ret;
	int i;

	if (!ddata || !ddata->regmap)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(wcd934x_diag_regs); i++) {
		ret = regmap_read(ddata->regmap, wcd934x_diag_regs[i].reg, &val);
		if (ret) {
			len += sysfs_emit_at(buf, len, "%-26s 0x%04x error=%d\n",
					     wcd934x_diag_regs[i].name,
					     wcd934x_diag_regs[i].reg, ret);
			continue;
		}

		len += sysfs_emit_at(buf, len, "%-26s 0x%04x 0x%02x\n",
				     wcd934x_diag_regs[i].name,
				     wcd934x_diag_regs[i].reg, val);
	}

	return len;
}
static DEVICE_ATTR_RO(diag_regs);

static ssize_t diag_poll_ms_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct wcd934x_diag_snapshot old, new;
	struct wcd934x_ddata *ddata = dev_get_drvdata(dev);
	unsigned int duration_ms;
	unsigned int elapsed_ms;
	unsigned int changes = 0;
	int ret;

	if (!ddata || !ddata->regmap)
		return -ENODEV;

	ret = kstrtouint(buf, 0, &duration_ms);
	if (ret)
		return ret;

	if (!duration_ms || duration_ms > 30000)
		return -EINVAL;

	dev_info(dev, "passive diag poll start duration=%ums step=20ms\n",
		 duration_ms);

	wcd934x_diag_read_snapshot(ddata, &old);
	wcd934x_diag_log_snapshot(ddata, "initial", 0, &old);

	for (elapsed_ms = 20; elapsed_ms <= duration_ms; elapsed_ms += 20) {
		msleep(20);
		wcd934x_diag_read_snapshot(ddata, &new);
		if (wcd934x_diag_snapshot_changed(ddata, elapsed_ms, &old, &new)) {
			old = new;
			changes++;
		}
	}

	dev_info(dev, "passive diag poll done changes=%u\n", changes);

	return count;
}
static DEVICE_ATTR_WO(diag_poll_ms);

static struct attribute *wcd934x_diag_attrs[] = {
	&dev_attr_diag_regs.attr,
	&dev_attr_diag_poll_ms.attr,
	NULL,
};

static const struct attribute_group wcd934x_diag_attr_group = {
	.attrs = wcd934x_diag_attrs,
};

static bool wcd934x_xiaomi_book12_passive_diag(struct slim_device *sdev)
{
	return of_machine_is_compatible("xiaomi,book-12.4") &&
		sdev->e_addr.manf_id == SLIM_MANF_ID_QCOM &&
		sdev->e_addr.prod_code == SLIM_PROD_CODE_WCD9340 &&
		sdev->e_addr.dev_index == SLIM_DEV_IDX_WCD9340 &&
		sdev->e_addr.instance == SLIM_DEV_INSTANCE_ID_WCD9340;
}

static int wcd934x_bring_up(struct wcd934x_ddata *ddata)
{
	struct regmap *regmap = ddata->regmap;
	u16 id_minor, id_major;
	int ret;

	ret = regmap_bulk_read(regmap, WCD934X_CHIP_TIER_CTRL_CHIP_ID_BYTE0,
			       (u8 *)&id_minor, sizeof(u16));
	if (ret)
		return ret;

	ret = regmap_bulk_read(regmap, WCD934X_CHIP_TIER_CTRL_CHIP_ID_BYTE2,
			       (u8 *)&id_major, sizeof(u16));
	if (ret)
		return ret;

	dev_info(ddata->dev, "WCD934x chip id major 0x%x, minor 0x%x\n",
		 id_major, id_minor);

	regmap_write(regmap, WCD934X_CODEC_RPM_RST_CTL, 0x01);
	regmap_write(regmap, WCD934X_SIDO_NEW_VOUT_A_STARTUP, 0x19);
	regmap_write(regmap, WCD934X_SIDO_NEW_VOUT_D_STARTUP, 0x15);
	/* Add 1msec delay for VOUT to settle */
	usleep_range(1000, 1100);
	regmap_write(regmap, WCD934X_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x5);
	regmap_write(regmap, WCD934X_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x7);
	regmap_write(regmap, WCD934X_CODEC_RPM_RST_CTL, 0x3);
	regmap_write(regmap, WCD934X_CODEC_RPM_RST_CTL, 0x7);
	regmap_write(regmap, WCD934X_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x3);

	return 0;
}

static int wcd934x_slim_status_up(struct slim_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct wcd934x_ddata *ddata;
	int ret;

	ddata = dev_get_drvdata(dev);

	ddata->regmap = regmap_init_slimbus(sdev, &wcd934x_regmap_config);
	if (IS_ERR(ddata->regmap)) {
		dev_err(dev, "Error allocating slim regmap\n");
		return PTR_ERR(ddata->regmap);
	}

	if (ddata->passive_diag) {
		ret = sysfs_create_group(&dev->kobj, &wcd934x_diag_attr_group);
		if (ret && ret != -EEXIST)
			return ret;

		ddata->diag_group_created = true;
		dev_info(dev, "Xiaomi passive diagnostic regmap ready; no bring-up, IRQ chip or MFD children\n");
		return 0;
	}

	ret = wcd934x_bring_up(ddata);
	if (ret) {
		dev_err(dev, "Failed to bring up WCD934X: err = %d\n", ret);
		return ret;
	}

	ret = devm_regmap_add_irq_chip(dev, ddata->regmap, ddata->irq,
				       IRQF_TRIGGER_HIGH, 0,
				       &wcd934x_regmap_irq_chip,
				       &ddata->irq_data);
	if (ret) {
		dev_err(dev, "Failed to add IRQ chip: err = %d\n", ret);
		return ret;
	}

	ret = mfd_add_devices(dev, PLATFORM_DEVID_AUTO, wcd934x_devices,
			      ARRAY_SIZE(wcd934x_devices), NULL, 0, NULL);
	if (ret) {
		dev_err(dev, "Failed to add child devices: err = %d\n",
			ret);
		return ret;
	}

	return ret;
}

static int wcd934x_slim_status(struct slim_device *sdev,
			       enum slim_device_status status)
{
	struct wcd934x_ddata *ddata = dev_get_drvdata(&sdev->dev);

	switch (status) {
	case SLIM_DEVICE_STATUS_UP:
		return wcd934x_slim_status_up(sdev);
	case SLIM_DEVICE_STATUS_DOWN:
		if (ddata && ddata->passive_diag) {
			if (ddata->diag_group_created) {
				sysfs_remove_group(&sdev->dev.kobj, &wcd934x_diag_attr_group);
				ddata->diag_group_created = false;
			}
			break;
		}

		mfd_remove_devices(&sdev->dev);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int wcd934x_slim_probe(struct slim_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct device_node *np = dev->of_node;
	struct wcd934x_ddata *ddata;
	struct gpio_desc *reset_gpio;
	int ret;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return	-ENOMEM;

	ddata->dev = dev;
	ddata->passive_diag = of_property_read_bool(np, "qcom,xiaomi-passive-diag") ||
			      wcd934x_xiaomi_book12_passive_diag(sdev);

	dev_info(dev, "probe of_node=%pOF passive_diag=%u eaddr=%x:%x:%x:%x\n",
		 np, ddata->passive_diag, sdev->e_addr.manf_id,
		 sdev->e_addr.prod_code, sdev->e_addr.dev_index,
		 sdev->e_addr.instance);

	if (ddata->passive_diag) {
		dev_set_drvdata(dev, ddata);
		dev_info(dev, "Xiaomi passive diagnostic mode; skipping IRQ, extclk, regulators and reset\n");
		return 0;
	}

	ddata->irq = of_irq_get(np, 0);
	if (ddata->irq < 0)
		return dev_err_probe(ddata->dev, ddata->irq,
				     "Failed to get IRQ\n");

	ddata->extclk = devm_clk_get(dev, "extclk");
	if (IS_ERR(ddata->extclk))
		return dev_err_probe(dev, PTR_ERR(ddata->extclk),
				     "Failed to get extclk");

	ddata->supplies[0].supply = "vdd-buck";
	ddata->supplies[1].supply = "vdd-buck-sido";
	ddata->supplies[2].supply = "vdd-tx";
	ddata->supplies[3].supply = "vdd-rx";
	ddata->supplies[4].supply = "vdd-io";

	ret = regulator_bulk_get(dev, WCD934X_MAX_SUPPLY, ddata->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");

	ret = regulator_bulk_enable(WCD934X_MAX_SUPPLY, ddata->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable supplies\n");

	/*
	 * For WCD934X, it takes about 600us for the Vout_A and
	 * Vout_D to be ready after BUCK_SIDO is powered up.
	 * SYS_RST_N shouldn't be pulled high during this time
	 */
	usleep_range(600, 650);
	reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio)) {
		ret = dev_err_probe(dev, PTR_ERR(reset_gpio),
				    "Failed to get reset gpio\n");
		goto err_disable_regulators;
	}
	msleep(20);
	gpiod_set_value(reset_gpio, 1);
	msleep(20);

	dev_set_drvdata(dev, ddata);

	return 0;

err_disable_regulators:
	regulator_bulk_disable(WCD934X_MAX_SUPPLY, ddata->supplies);
	return ret;
}

static void wcd934x_slim_remove(struct slim_device *sdev)
{
	struct wcd934x_ddata *ddata = dev_get_drvdata(&sdev->dev);

	if (ddata && ddata->passive_diag) {
		if (ddata->diag_group_created)
			sysfs_remove_group(&sdev->dev.kobj, &wcd934x_diag_attr_group);
		return;
	}

	regulator_bulk_disable(WCD934X_MAX_SUPPLY, ddata->supplies);
	mfd_remove_devices(&sdev->dev);
}

static const struct slim_device_id wcd934x_slim_id[] = {
	{ SLIM_MANF_ID_QCOM, SLIM_PROD_CODE_WCD9340,
	  SLIM_DEV_IDX_WCD9340, SLIM_DEV_INSTANCE_ID_WCD9340 },
	{}
};
MODULE_DEVICE_TABLE(slim, wcd934x_slim_id);

static struct slim_driver wcd934x_slim_driver = {
	.driver = {
		.name = "wcd934x-slim",
	},
	.probe = wcd934x_slim_probe,
	.remove = wcd934x_slim_remove,
	.device_status = wcd934x_slim_status,
	.id_table = wcd934x_slim_id,
};

module_slim_driver(wcd934x_slim_driver);
MODULE_DESCRIPTION("WCD934X slim driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org>");
