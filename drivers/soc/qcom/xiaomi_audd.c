// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi Book 12.4 Qualcomm AUDD diagnostic driver.
 *
 * Windows exposes \_SB.ADSP.SLM1.ADCM.AUDD on SPI4/CS0 with GPIO resources.
 * The AUDD GpioInt pin 0x0100 is allocated by Windows qcgpio/GPIOClx as
 * ADCM IRQ1055, not as a normal TLMM GPIO interrupt that Linux can map today.
 * This driver intentionally performs no SPI transfers. It only verifies that
 * the DT node can bind and that the GPIO lines can be requested. Optional WSA
 * SD_N GPIOs are exposed through explicit sysfs diagnostics and are never
 * driven during probe.
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#define XIAOMI_AUDD_MAX_IRQS	8
#define XIAOMI_AUDD_MAX_WSA_SD_N_GPIOS	2

struct xiaomi_audd_irq {
	const char *name;
	int irq;
};

struct xiaomi_audd {
	struct device *dev;
	struct gpio_desc *audd_gpio;
	struct gpio_desc *mbhc_gpio;
	struct device_node *soundwire_np;
	struct xiaomi_audd_irq irqs[XIAOMI_AUDD_MAX_IRQS];
	unsigned int num_irqs;
	struct gpio_desc *wsa_sd_n_gpios[XIAOMI_AUDD_MAX_WSA_SD_N_GPIOS];
	unsigned int num_wsa_sd_n_gpios;
	struct mutex irq_lock;
	struct mutex wsa_sd_n_lock;
	atomic_t irq_count;
	atomic_t irq_disabled;
	unsigned long irq_start;
	unsigned long irq_first;
};

static int xiaomi_audd_gpio_value(struct gpio_desc *gpiod)
{
	if (!gpiod)
		return -ENOENT;

	return gpiod_get_value_cansleep(gpiod);
}

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

static int xiaomi_audd_gpio_raw_value(struct gpio_desc *gpiod)
{
	if (!gpiod)
		return -ENOENT;

	return gpiod_get_raw_value_cansleep(gpiod);
}

static int xiaomi_audd_wsa_sd_n_raw_set_one(struct xiaomi_audd *audd,
					    unsigned int index, int value)
{
	struct device *dev = audd->dev;
	int ret;

	if (!audd->num_wsa_sd_n_gpios)
		return -ENOENT;

	if (index >= audd->num_wsa_sd_n_gpios)
		return -EINVAL;

	ret = gpiod_direction_output_raw(audd->wsa_sd_n_gpios[index], value);
	if (ret) {
		dev_info(dev, "WSA SD_N gpio[%u] raw set %d failed: %d\n",
			 index, value, ret);
		return ret;
	}

	usleep_range(2000, 3000);

	return 0;
}

static int xiaomi_audd_wsa_sd_n_raw_set_all(struct xiaomi_audd *audd, int value)
{
	struct device *dev = audd->dev;
	unsigned int i;
	int ret;

	if (!audd->num_wsa_sd_n_gpios)
		return -ENOENT;

	for (i = 0; i < audd->num_wsa_sd_n_gpios; i++) {
		ret = gpiod_direction_output_raw(audd->wsa_sd_n_gpios[i], value);
		if (ret) {
			dev_info(dev, "WSA SD_N gpio[%u] raw set %d failed: %d\n",
				 i, value, ret);
			return ret;
		}
	}

	usleep_range(2000, 3000);

	return 0;
}

static void xiaomi_audd_of_node_put(void *data)
{
	of_node_put(data);
}

static struct device *xiaomi_audd_soundwire_get(struct xiaomi_audd *audd)
{
	struct platform_device *pdev;
	struct device *sw_dev;
	int ret;

	if (!audd->soundwire_np)
		return NULL;

	pdev = of_find_device_by_node(audd->soundwire_np);
	if (!pdev) {
		dev_info(audd->dev, "SoundWire diagnostic target %pOF is not bound\n",
			 audd->soundwire_np);
		return ERR_PTR(-ENODEV);
	}

	sw_dev = &pdev->dev;
	ret = pm_runtime_resume_and_get(sw_dev);
	if (ret < 0) {
		dev_info(audd->dev, "SoundWire diagnostic target %s resume failed: %d\n",
			 dev_name(sw_dev), ret);
		put_device(sw_dev);
		return ERR_PTR(ret);
	}

	dev_info(audd->dev, "SoundWire diagnostic target %s held active\n",
		 dev_name(sw_dev));

	return sw_dev;
}

static void xiaomi_audd_soundwire_put(struct device *sw_dev)
{
	if (!sw_dev || IS_ERR(sw_dev))
		return;

	pm_runtime_mark_last_busy(sw_dev);
	pm_runtime_put_autosuspend(sw_dev);
	put_device(sw_dev);
}

static irqreturn_t xiaomi_audd_irq_handler(int irq, void *data)
{
	struct xiaomi_audd *audd = data;

	if (atomic_inc_return(&audd->irq_count) == 1)
		WRITE_ONCE(audd->irq_first, jiffies);

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

static ssize_t gpio_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct xiaomi_audd *audd = dev_get_drvdata(dev);
	int audd_value = xiaomi_audd_gpio_value(audd->audd_gpio);
	int mbhc_value = xiaomi_audd_gpio_value(audd->mbhc_gpio);

	return sysfs_emit(buf, "audd=%d\nmbhc=%d\n", audd_value, mbhc_value);
}
static DEVICE_ATTR_RO(gpio_state);

static ssize_t wsa_sd_n_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct xiaomi_audd *audd = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int i;

	if (!audd->num_wsa_sd_n_gpios)
		return sysfs_emit(buf, "none\n");

	len += sysfs_emit_at(buf, len, "count=%u\n", audd->num_wsa_sd_n_gpios);
	if (audd->soundwire_np)
		len += sysfs_emit_at(buf, len, "soundwire=%pOF\n",
				     audd->soundwire_np);

	for (i = 0; i < audd->num_wsa_sd_n_gpios; i++)
		len += sysfs_emit_at(buf, len, "wsa%u raw=%d logical=%d active_low=%d\n",
				     i,
				     xiaomi_audd_gpio_raw_value(audd->wsa_sd_n_gpios[i]),
				     xiaomi_audd_gpio_value(audd->wsa_sd_n_gpios[i]),
				     gpiod_is_active_low(audd->wsa_sd_n_gpios[i]));

	return len;
}
static DEVICE_ATTR_RO(wsa_sd_n_state);

/*
 * wsa_sd_n_raw accepts either "<raw>" for all described SD_N lines or
 * "<index> <raw>" for one line. wsa_sd_n_hold_ms accepts the matching
 * "<raw> <duration_ms>" and "<index> <raw> <duration_ms>" forms.
 */
static ssize_t wsa_sd_n_raw_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct xiaomi_audd *audd = dev_get_drvdata(dev);
	bool set_all;
	unsigned int arg0;
	unsigned int arg1;
	unsigned int index;
	unsigned int value;
	char extra;
	int ret;
	int fields;

	fields = sscanf(buf, "%u %u %c", &arg0, &arg1, &extra);
	if (fields == 1) {
		set_all = true;
		value = arg0;
		index = 0;
	} else if (fields == 2) {
		set_all = false;
		index = arg0;
		value = arg1;
	} else {
		return -EINVAL;
	}

	if (value > 1)
		return -EINVAL;

	mutex_lock(&audd->wsa_sd_n_lock);

	if (set_all) {
		ret = xiaomi_audd_wsa_sd_n_raw_set_all(audd, value);
		if (!ret)
			dev_info(dev, "WSA SD_N raw set all to %u\n", value);
	} else {
		ret = xiaomi_audd_wsa_sd_n_raw_set_one(audd, index, value);
		if (!ret)
			dev_info(dev, "WSA SD_N gpio[%u] raw set to %u\n",
				 index, value);
	}

	mutex_unlock(&audd->wsa_sd_n_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(wsa_sd_n_raw);

static ssize_t wsa_sd_n_hold_ms_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct xiaomi_audd *audd = dev_get_drvdata(dev);
	struct device *sw_dev = NULL;
	int previous[XIAOMI_AUDD_MAX_WSA_SD_N_GPIOS];
	bool set_all;
	unsigned int arg0;
	unsigned int arg1;
	unsigned int arg2;
	unsigned int duration_ms;
	unsigned int index;
	unsigned int value;
	unsigned int i;
	char extra;
	int ret;
	int fields;

	fields = sscanf(buf, "%u %u %u %c", &arg0, &arg1, &arg2, &extra);
	if (fields == 2) {
		set_all = true;
		value = arg0;
		duration_ms = arg1;
		index = 0;
	} else if (fields == 3) {
		set_all = false;
		index = arg0;
		value = arg1;
		duration_ms = arg2;
	} else {
		return -EINVAL;
	}

	if (value > 1 || !duration_ms || duration_ms > 30000)
		return -EINVAL;

	mutex_lock(&audd->wsa_sd_n_lock);

	if (!audd->num_wsa_sd_n_gpios) {
		ret = -ENOENT;
		goto out_unlock;
	}

	if (!set_all && index >= audd->num_wsa_sd_n_gpios) {
		ret = -EINVAL;
		goto out_unlock;
	}

	for (i = 0; i < audd->num_wsa_sd_n_gpios; i++) {
		previous[i] = xiaomi_audd_gpio_raw_value(audd->wsa_sd_n_gpios[i]);
		if (previous[i] < 0) {
			ret = previous[i];
			goto out_unlock;
		}
	}

	if (set_all) {
		dev_info(dev, "WSA SD_N raw asserting all to %u before SoundWire resume\n",
			 value);

		ret = xiaomi_audd_wsa_sd_n_raw_set_all(audd, value);
		if (ret)
			goto out_unlock;

		dev_info(dev, "WSA SD_N raw holding all at %u for %u ms\n",
			 value, duration_ms);
	} else {
		dev_info(dev, "WSA SD_N gpio[%u] raw asserting %u before SoundWire resume\n",
			 index, value);

		ret = xiaomi_audd_wsa_sd_n_raw_set_one(audd, index, value);
		if (ret)
			goto out_unlock;

		dev_info(dev, "WSA SD_N gpio[%u] raw holding %u for %u ms\n",
			 index, value, duration_ms);
	}

	sw_dev = xiaomi_audd_soundwire_get(audd);
	if (IS_ERR(sw_dev)) {
		ret = PTR_ERR(sw_dev);
		goto out_restore;
	}

	msleep(duration_ms);

out_restore:
	for (i = 0; i < audd->num_wsa_sd_n_gpios; i++) {
		if (!set_all && i != index)
			continue;

		ret = gpiod_direction_output_raw(audd->wsa_sd_n_gpios[i],
						 previous[i]);
		if (ret) {
			dev_info(dev, "WSA SD_N gpio[%u] restore raw %d failed: %d\n",
				 i, previous[i], ret);
			goto out_put_soundwire;
		}
	}

	dev_info(dev, "WSA SD_N raw state restored after hold\n");

out_put_soundwire:
	xiaomi_audd_soundwire_put(sw_dev);
out_unlock:
	mutex_unlock(&audd->wsa_sd_n_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(wsa_sd_n_hold_ms);

static ssize_t gpio_poll_ms_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct xiaomi_audd *audd = dev_get_drvdata(dev);
	unsigned int duration_ms;
	unsigned long start;
	unsigned long end;
	int audd_value;
	int mbhc_value;
	int audd_last;
	int mbhc_last;
	int changes = 0;
	int ret;

	ret = kstrtouint(buf, 0, &duration_ms);
	if (ret)
		return ret;

	if (!duration_ms || duration_ms > 30000)
		return -EINVAL;

	audd_last = xiaomi_audd_gpio_value(audd->audd_gpio);
	mbhc_last = xiaomi_audd_gpio_value(audd->mbhc_gpio);
	start = jiffies;
	end = start + msecs_to_jiffies(duration_ms);

	dev_info(dev, "GPIO poll start duration=%u ms audd=%d mbhc=%d\n",
		 duration_ms, audd_last, mbhc_last);

	while (time_before(jiffies, end)) {
		msleep(20);

		audd_value = xiaomi_audd_gpio_value(audd->audd_gpio);
		mbhc_value = xiaomi_audd_gpio_value(audd->mbhc_gpio);

		if (audd_value != audd_last || mbhc_value != mbhc_last) {
			changes++;
			dev_info(dev,
				 "GPIO poll change[%d] elapsed_ms=%u audd=%d->%d mbhc=%d->%d\n",
				 changes, jiffies_to_msecs(jiffies - start),
				 audd_last, audd_value, mbhc_last, mbhc_value);
			audd_last = audd_value;
			mbhc_last = mbhc_value;
		}
	}

	dev_info(dev, "GPIO poll done changes=%d audd=%d mbhc=%d\n",
		 changes, audd_last, mbhc_last);

	return count;
}
static DEVICE_ATTR_WO(gpio_poll_ms);

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
	unsigned long first;
	unsigned long start;
	int irq_count;
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
	WRITE_ONCE(audd->irq_start, jiffies);
	WRITE_ONCE(audd->irq_first, 0);

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

	irq_count = atomic_read(&audd->irq_count);
	first = READ_ONCE(audd->irq_first);
	start = READ_ONCE(audd->irq_start);
	if (irq_count)
		dev_info(dev, "IRQ test %s irq=%d count=%d first_ms=%u\n",
			 audd->irqs[index].name, audd->irqs[index].irq,
			 irq_count, jiffies_to_msecs(first - start));
	else
		dev_info(dev, "IRQ test %s irq=%d count=0\n",
			 audd->irqs[index].name, audd->irqs[index].irq);

out_unlock:
	mutex_unlock(&audd->irq_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(irq_test_ms);

static struct attribute *xiaomi_audd_attrs[] = {
	&dev_attr_gpio_state.attr,
	&dev_attr_gpio_poll_ms.attr,
	&dev_attr_irq_candidates.attr,
	&dev_attr_irq_test_ms.attr,
	&dev_attr_wsa_sd_n_state.attr,
	&dev_attr_wsa_sd_n_raw.attr,
	&dev_attr_wsa_sd_n_hold_ms.attr,
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

static int xiaomi_audd_collect_wsa_sd_n_gpios(struct device *dev,
					      struct xiaomi_audd *audd)
{
	unsigned int i;

	for (i = 0; i < XIAOMI_AUDD_MAX_WSA_SD_N_GPIOS; i++) {
		struct gpio_desc *gpiod;
		int logical;
		int raw;

		gpiod = devm_gpiod_get_index_optional(dev, "wsa-sd-n", i,
						      GPIOD_ASIS);
		if (IS_ERR(gpiod))
			return PTR_ERR(gpiod);
		if (!gpiod)
			break;

		raw = xiaomi_audd_gpio_raw_value(gpiod);
		logical = xiaomi_audd_gpio_value(gpiod);

		audd->wsa_sd_n_gpios[audd->num_wsa_sd_n_gpios++] = gpiod;
		dev_info(dev,
			 "WSA SD_N gpio[%u] requested, raw=%d logical=%d active_low=%d\n",
			 i, raw, logical, gpiod_is_active_low(gpiod));
	}

	if (!audd->num_wsa_sd_n_gpios)
		dev_info(dev, "WSA SD_N gpios not described\n");

	return 0;
}

static int xiaomi_audd_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct xiaomi_audd *audd;
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
	mutex_init(&audd->wsa_sd_n_lock);
	dev_set_drvdata(dev, audd);

	audd->soundwire_np = of_parse_phandle(dev->of_node, "qcom,soundwire", 0);
	if (audd->soundwire_np) {
		ret = devm_add_action_or_reset(dev, xiaomi_audd_of_node_put,
					       audd->soundwire_np);
		if (ret)
			return ret;

		dev_info(dev, "SoundWire diagnostic target %pOF\n",
			 audd->soundwire_np);
	} else {
		dev_info(dev, "SoundWire diagnostic target not described\n");
	}

	ret = xiaomi_audd_collect_irqs(spi, audd);
	if (ret)
		return dev_err_probe(dev, ret, "failed to collect IRQ candidates\n");

	if (!audd->num_irqs)
		dev_info(dev,
			 "no IRQ mapped; Windows qcgpio allocates AUDD GpioInt 0x0100 as ADCM IRQ1055\n");

	audd->audd_gpio = devm_gpiod_get_optional(dev, "audd", GPIOD_ASIS);
	if (IS_ERR(audd->audd_gpio))
		return dev_err_probe(dev, PTR_ERR(audd->audd_gpio),
				     "failed to request audd gpio\n");

	audd->mbhc_gpio = devm_gpiod_get_optional(dev, "mbhc", GPIOD_ASIS);
	if (IS_ERR(audd->mbhc_gpio))
		return dev_err_probe(dev, PTR_ERR(audd->mbhc_gpio),
				     "failed to request mbhc gpio\n");

	xiaomi_audd_log_gpio(dev, "audd", audd->audd_gpio);
	xiaomi_audd_log_gpio(dev, "mbhc", audd->mbhc_gpio);

	ret = xiaomi_audd_collect_wsa_sd_n_gpios(dev, audd);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request WSA SD_N gpios\n");

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
