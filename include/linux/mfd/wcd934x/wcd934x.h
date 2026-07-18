/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __WCD934X_H__
#define __WCD934X_H__
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/slimbus.h>

#define WCD934X_MAX_SUPPLY	5

/**
 * struct wcd934x_ddata - wcd934x driver data
 *
 * @supplies:	wcd934x regulator supplies
 * @irq_data:	wcd934x irq_chip data
 * @regmap:	wcd934x regmap pointer
 * @extclk:	External clock
 * @dev:	device instance of wcd934x slim device
 * @irq:	irq for wcd934x.
 * @passive_diag: read-only diagnostic mode without IRQ/MFD children.
 * @diag_group_created: passive diagnostic sysfs group is live.
 */
struct wcd934x_ddata {
	struct regulator_bulk_data supplies[WCD934X_MAX_SUPPLY];
	struct regmap_irq_chip_data *irq_data;
	struct regmap *regmap;
	struct clk *extclk;
	struct device *dev;
	int irq;
	bool passive_diag;
	bool diag_group_created;
};

#endif /* __WCD934X_H__ */
