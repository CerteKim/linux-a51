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
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#define XIAOMI_AUDD_MAX_IRQS	8

struct xiaomi_audd_irq {
	const char *name;
	int irq;
};

struct xiaomi_audd {
	struct device *dev;
	struct xiaomi_audd_irq irqs[XIAOMI_AUDD_MAX_IRQS];
	unsigned int num_irqs;
	struct mutex irq_lock;
	atomic_t irq_count;
	atomic_t irq_disabled;
};

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

static irqreturn_t xiaomi_audd_irq_handler(int irq, void *data)
{
	struct xiaomi_audd *audd = data;

	atomic_inc(&audd->irq_count);
	if (!atomic_xchg(&audd->irq_disabled, 1))
		disable_irq_nosync(irq);

	return IRQ_HANDLED;
}

static ssize_t irq_candidates_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct xiaomi_audd *audd = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int i;

	if (!audd->num_irqs)
		return sysfs_emit(buf, "none\n");

	for (i = 0; i < audd->num_irqs; i++)
		len += sysfs_emit_at(buf, len, "%u %s irq=%d\n", i,
				     audd->irqs[i].name, audd->irqs[i].irq);

	return len;
}
static DEVICE_ATTR_RO(irq_candidates);

static int xiaomi_audd_irq_index(struct xiaomi_audd *audd, const char *token)
{
	unsigned int i;
	u32 index;

	if (!kstrtou32(token, 0, &index)) {
		if (index < audd->num_irqs)
			return index;
		return -EINVAL;
	}

	for (i = 0; i < audd->num_irqs; i++) {
		if (!strcmp(audd->irqs[i].name, token))
			return i;
	}

	return -EINVAL;
}

static ssize_t irq_test_ms_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct xiaomi_audd *audd = dev_get_drvdata(dev);
	char token[64];
	unsigned int duration_ms;
	int index;
	int ret;

	if (sscanf(buf, "%63s %u", token, &duration_ms) != 2)
		return -EINVAL;

	if (!duration_ms || duration_ms > 30000)
		return -EINVAL;

	index = xiaomi_audd_irq_index(audd, token);
	if (index < 0)
		return index;

	mutex_lock(&audd->irq_lock);

	atomic_set(&audd->irq_count, 0);
	atomic_set(&audd->irq_disabled, 0);

	ret = request_irq(audd->irqs[index].irq, xiaomi_audd_irq_handler,
			  IRQF_NO_AUTOEN, dev_name(dev), audd);
	if (ret) {
		dev_info(dev, "IRQ test %s irq=%d request failed: %d\n",
			 audd->irqs[index].name, audd->irqs[index].irq, ret);
		goto out_unlock;
	}

	dev_info(dev, "IRQ test %s irq=%d enabled for %u ms\n",
		 audd->irqs[index].name, audd->irqs[index].irq, duration_ms);

	enable_irq(audd->irqs[index].irq);
	msleep(duration_ms);

	free_irq(audd->irqs[index].irq, audd);

	if (atomic_read(&audd->irq_disabled))
		enable_irq(audd->irqs[index].irq);

	dev_info(dev, "IRQ test %s irq=%d count=%d\n",
		 audd->irqs[index].name, audd->irqs[index].irq,
		 atomic_read(&audd->irq_count));

out_unlock:
	mutex_unlock(&audd->irq_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(irq_test_ms);

static struct attribute *xiaomi_audd_attrs[] = {
	&dev_attr_irq_candidates.attr,
	&dev_attr_irq_test_ms.attr,
	NULL,
};

static const struct attribute_group xiaomi_audd_group = {
	.attrs = xiaomi_audd_attrs,
};

static int xiaomi_audd_collect_irqs(struct spi_device *spi,
				    struct xiaomi_audd *audd)
{
	struct device *dev = &spi->dev;
	struct device_node *np = dev->of_node;
	int irq_count;
	int i;

	irq_count = of_irq_count(np);
	if (irq_count < 0)
		return irq_count;

	if (irq_count > XIAOMI_AUDD_MAX_IRQS) {
		dev_info(dev, "limiting %d IRQ candidates to %u\n",
			 irq_count, XIAOMI_AUDD_MAX_IRQS);
		irq_count = XIAOMI_AUDD_MAX_IRQS;
	}

	for (i = 0; i < irq_count; i++) {
		const char *name;
		int irq;

		irq = of_irq_get(np, i);
		if (irq == -EPROBE_DEFER)
			return irq;
		if (irq < 0) {
			dev_info(dev, "IRQ candidate[%d] mapping failed: %d\n", i, irq);
			continue;
		}

		if (of_property_read_string_index(np, "interrupt-names", i, &name))
			name = devm_kasprintf(dev, GFP_KERNEL, "irq%d", i);
		if (!name)
			return -ENOMEM;

		audd->irqs[audd->num_irqs].name = name;
		audd->irqs[audd->num_irqs].irq = irq;
		dev_info(dev, "IRQ candidate[%u] %s -> irq=%d\n",
			 audd->num_irqs, name, irq);
		audd->num_irqs++;
	}

	return 0;
}

static int xiaomi_audd_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct xiaomi_audd *audd;
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

	audd = devm_kzalloc(dev, sizeof(*audd), GFP_KERNEL);
	if (!audd)
		return -ENOMEM;

	audd->dev = dev;
	mutex_init(&audd->irq_lock);
	dev_set_drvdata(dev, audd);

	ret = xiaomi_audd_collect_irqs(spi, audd);
	if (ret)
		return dev_err_probe(dev, ret, "failed to collect IRQ candidates\n");

	if (!audd->num_irqs)
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

	ret = devm_device_add_group(dev, &xiaomi_audd_group);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add diagnostic sysfs attributes\n");

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
