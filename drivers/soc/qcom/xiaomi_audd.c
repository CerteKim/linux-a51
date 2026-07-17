// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi Book 12.4 Qualcomm AUDD diagnostic driver.
 *
 * Windows exposes \_SB.ADSP.SLM1.ADCM.AUDD on SPI4/CS0 with GPIO resources.
 * This driver intentionally performs no SPI transfers and does not drive any
 * GPIO. It only verifies that the DT node can bind and that the GPIO lines can
 * be requested without changing their direction.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
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

	dev_info(dev,
		 "probe on SPI bus=%d cs=%u mode=0x%x bits_per_word=%u max_speed=%u irq=%d node=%pOF\n",
		 spi->controller->bus_num, spi_get_chipselect(spi, 0),
		 spi->mode, spi->bits_per_word, spi->max_speed_hz, spi->irq,
		 dev->of_node);

	if (!spi->irq)
		dev_info(dev, "no IRQ mapped; Windows AUDD GpioInt 0x0100 remains unresolved\n");

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
	dev_info(dev, "diagnostic probe complete, no SPI transfer was issued\n");

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
module_spi_driver(xiaomi_audd_driver);

MODULE_DESCRIPTION("Xiaomi Book 12.4 Qualcomm AUDD diagnostic driver");
MODULE_LICENSE("GPL");
