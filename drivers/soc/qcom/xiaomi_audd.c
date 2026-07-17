// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi Book 12.4 Qualcomm AUDD diagnostic driver.
 *
 * Windows exposes \_SB.ADSP.SLM1.ADCM.AUDD on SPI4/CS0 with GPIO resources.
 * The AUDD GpioInt pin 0x0100 is allocated by Windows qcgpio/GPIOClx as
 * ADCM IRQ1055, not as a normal TLMM GPIO interrupt that Linux can map today.
 * This driver intentionally performs no SPI transfers and does not drive any
 * GPIO. It only verifies that the DT node can bind and that the GPIO lines can
 * be requested without changing their direction.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

static void xiaomi_audd_log_gpio(struct device *dev, const char *name,
				 struct gpio_desc *gpiod)
{
	int value;

	if (!gpiod) {
		dev_info(dev, "%s gpio not described\n", name);
		return;
	}

	value = gpiod_get_value_cansleep(gpiod);
	if (value < 0)
		dev_info(dev, "%s gpio requested, read failed: %d\n", name, value);
	else
		dev_info(dev, "%s gpio requested, current value=%d\n", name, value);
}

static int xiaomi_audd_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct gpio_desc *audd_gpio;
	struct gpio_desc *mbhc_gpio;
	struct device_node *child;
	unsigned int child_count = 0;
	int ret;

	dev_info(dev,
		 "probe on SPI bus=%d cs=%u mode=0x%x bits_per_word=%u max_speed=%u irq=%d node=%pOF\n",
		 spi->controller->bus_num, spi_get_chipselect(spi, 0),
		 spi->mode, spi->bits_per_word, spi->max_speed_hz, spi->irq,
		 dev->of_node);

	if (!spi->irq)
		dev_info(dev,
			 "no IRQ mapped; Windows qcgpio allocates AUDD GpioInt 0x0100 as ADCM IRQ1055\n");

	audd_gpio = devm_gpiod_get_optional(dev, "audd", GPIOD_ASIS);
	if (IS_ERR(audd_gpio))
		return dev_err_probe(dev, PTR_ERR(audd_gpio),
				     "failed to request audd gpio\n");

	mbhc_gpio = devm_gpiod_get_optional(dev, "mbhc", GPIOD_ASIS);
	if (IS_ERR(mbhc_gpio))
		return dev_err_probe(dev, PTR_ERR(mbhc_gpio),
				     "failed to request mbhc gpio\n");

	xiaomi_audd_log_gpio(dev, "audd", audd_gpio);
	xiaomi_audd_log_gpio(dev, "mbhc", mbhc_gpio);

	for_each_available_child_of_node(dev->of_node, child) {
		const char *hid = NULL;
		const char *compat = NULL;
		u32 reg;

		of_property_read_string(child, "compatible", &compat);
		of_property_read_string(child, "qcom,windows-hid", &hid);

		if (!of_property_read_u32(child, "reg", &reg))
			dev_info(dev, "child[%u] reg=%u compatible=%s windows-hid=%s\n",
				 child_count, reg, compat ?: "(none)", hid ?: "(none)");
		else
			dev_info(dev, "child[%u] compatible=%s windows-hid=%s\n",
				 child_count, compat ?: "(none)", hid ?: "(none)");

		child_count++;
	}

	ret = devm_of_platform_populate(dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to populate passive AUDD child devices\n");

	dev_info(dev,
		 "diagnostic probe complete, enumerated %u child device(s), no SPI transfer was issued\n",
		 child_count);

	return 0;
}

static int xiaomi_audd_child_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *function = of_device_get_match_data(dev);
	const char *hid = NULL;
	u32 reg;

	of_property_read_string(dev->of_node, "qcom,windows-hid", &hid);

	if (!of_property_read_u32(dev->of_node, "reg", &reg))
		dev_info(dev, "passive %s child bound: reg=%u windows-hid=%s\n",
			 function ?: "AUDD", reg, hid ?: "(none)");
	else
		dev_info(dev, "passive %s child bound: windows-hid=%s\n",
			 function ?: "AUDD", hid ?: "(none)");

	return 0;
}

static const struct of_device_id xiaomi_audd_of_match[] = {
	{ .compatible = "qcom,sc8180x-xiaomi-audd" },
	{ }
};
MODULE_DEVICE_TABLE(of, xiaomi_audd_of_match);

static const struct spi_device_id xiaomi_audd_spi_ids[] = {
	{ "sc8180x-xiaomi-audd" },
	{ }
};
MODULE_DEVICE_TABLE(spi, xiaomi_audd_spi_ids);

static struct spi_driver xiaomi_audd_driver = {
	.probe = xiaomi_audd_probe,
	.id_table = xiaomi_audd_spi_ids,
	.driver = {
		.name = "qcom-xiaomi-audd",
		.of_match_table = xiaomi_audd_of_match,
	},
};

static const struct of_device_id xiaomi_audd_child_of_match[] = {
	{
		.compatible = "qcom,sc8180x-xiaomi-audd-mbhc",
		.data = "MBHC",
	},
	{
		.compatible = "qcom,sc8180x-xiaomi-audd-adapter",
		.data = "audio adapter",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, xiaomi_audd_child_of_match);

static struct platform_driver xiaomi_audd_child_driver = {
	.probe = xiaomi_audd_child_probe,
	.driver = {
		.name = "qcom-xiaomi-audd-child",
		.of_match_table = xiaomi_audd_child_of_match,
	},
};

static int __init xiaomi_audd_init(void)
{
	int ret;

	ret = platform_driver_register(&xiaomi_audd_child_driver);
	if (ret)
		return ret;

	ret = spi_register_driver(&xiaomi_audd_driver);
	if (ret)
		platform_driver_unregister(&xiaomi_audd_child_driver);

	return ret;
}
module_init(xiaomi_audd_init);

static void __exit xiaomi_audd_exit(void)
{
	spi_unregister_driver(&xiaomi_audd_driver);
	platform_driver_unregister(&xiaomi_audd_child_driver);
}
module_exit(xiaomi_audd_exit);

MODULE_DESCRIPTION("Xiaomi Book 12.4 Qualcomm AUDD diagnostic driver");
MODULE_LICENSE("GPL");
