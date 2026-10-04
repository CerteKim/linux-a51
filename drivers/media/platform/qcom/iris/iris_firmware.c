// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/soc/qcom/mdt_loader.h>

#include "iris_core.h"
#include "iris_vpu_common.h"
#include "iris_firmware.h"

#define MAX_FIRMWARE_NAME_SIZE	128

/*
 * DEBUG: the firmware carve-out address is what TZ validates in
 * qcom_scm_pas_mem_setup(); being able to move it lets us find the address the
 * platform's TZ actually expects without rebuilding the DTB and rebooting.
 * Only point these at reserved (no-map) memory.
 */
static unsigned int iris_pas_id_override;
module_param_named(pas_id, iris_pas_id_override, uint, 0644);
MODULE_PARM_DESC(pas_id, "IRIS debug: override the PAS id for firmware load/auth");

static char *iris_fw_name_override;
module_param_named(fw_name, iris_fw_name_override, charp, 0644);
MODULE_PARM_DESC(fw_name, "IRIS debug: override the firmware file name");

static int iris_scan_pas(const char *val, const struct kernel_param *kp)
{
	unsigned int i;

	pr_info("IRIS-TRACE: TZ PAS support scan\n");
	for (i = 0; i < 32; i++)
		pr_info("IRIS-TRACE: PAS %2u supported=%d\n", i,
			qcom_scm_pas_supported(i));
	return 0;
}

static const struct kernel_param_ops iris_scan_pas_ops = {
	.set = iris_scan_pas,
};
module_param_cb(scan_pas, &iris_scan_pas_ops, NULL, 0200);

static bool iris_no_auth;
module_param_named(no_auth, iris_no_auth, bool, 0644);
MODULE_PARM_DESC(no_auth, "IRIS debug: stop after image init (no auth/reset)");

static unsigned long long iris_fw_phys_override;
module_param_named(fw_phys, iris_fw_phys_override, ullong, 0644);
MODULE_PARM_DESC(fw_phys, "IRIS debug: override firmware carve-out physical address");

static unsigned long long iris_fw_size_override;
module_param_named(fw_size, iris_fw_size_override, ullong, 0644);
MODULE_PARM_DESC(fw_size, "IRIS debug: override firmware carve-out size");

static u32 iris_pas_id(struct iris_core *core)
{
	return iris_pas_id_override ? iris_pas_id_override
				    : core->iris_platform_data->pas_id;
}

static int iris_load_fw_to_memory(struct iris_core *core, const char *fw_name)
{
	u32 pas_id = iris_pas_id(core);
	const struct firmware *firmware = NULL;
	struct device *dev = core->dev;
	struct reserved_mem *rmem;
	struct device_node *node;
	phys_addr_t mem_phys;
	size_t res_size;
	ssize_t fw_size;
	void *mem_virt;
	int ret;

	if (strlen(fw_name) >= MAX_FIRMWARE_NAME_SIZE - 4)
		return -EINVAL;

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node)
		return -EINVAL;

	rmem = of_reserved_mem_lookup(node);
	of_node_put(node);
	if (!rmem)
		return -EINVAL;

	mem_phys = rmem->base;
	res_size = rmem->size;
	if (iris_fw_phys_override)
		mem_phys = iris_fw_phys_override;
	if (iris_fw_size_override)
		res_size = iris_fw_size_override;
	dev_err(dev, "IRIS-TRACE: fw region phys=%pa size=%zu\n", &mem_phys, res_size);

	ret = request_firmware(&firmware, fw_name, dev);
	if (ret)
		return ret;

	dev_err(dev, "IRIS-TRACE: fw request ok, file size %zu\n", firmware->size);
	fw_size = qcom_mdt_get_size(firmware);
	if (fw_size < 0 || res_size < (size_t)fw_size) {
		ret = -EINVAL;
		goto err_release_fw;
	}

	mem_virt = memremap(mem_phys, res_size, MEMREMAP_WC);
	if (!mem_virt) {
		ret = -ENOMEM;
		goto err_release_fw;
	}

	dev_err(dev, "IRIS-TRACE: qcom_mdt_load (elf=%d) starting\n", fw_size);
	ret = qcom_mdt_load(dev, firmware, fw_name,
			    pas_id, mem_virt, mem_phys, res_size, NULL);
	dev_err(dev, "IRIS-TRACE: qcom_mdt_load ret=%d\n", ret);

	memunmap(mem_virt);
err_release_fw:
	release_firmware(firmware);

	return ret;
}

int iris_fw_load(struct iris_core *core)
{
	struct tz_cp_config *cp_config = core->iris_platform_data->tz_cp_config_data;
	const char *fwpath = NULL;
	int ret;

	ret = of_property_read_string_index(core->dev->of_node, "firmware-name", 0,
					    &fwpath);
	if (ret)
		fwpath = core->iris_platform_data->fwname;

	if (iris_fw_name_override)
		fwpath = iris_fw_name_override;

	dev_err(core->dev, "IRIS-TRACE: fw_load: name %s\n", fwpath);

	/*
	 * hfi_venus.c does this in venus_set_hw_state() for the TZ-managed
	 * firmware (use_tz), i.e. before the image is loaded and authenticated.
	 * It is what tells TZ to bring the remote subsystem up.
	 */
	dev_err(core->dev, "IRIS-TRACE: set remote state (SCM call)\n");
	ret = qcom_scm_set_remote_state(1, 0);
	dev_err(core->dev, "IRIS-TRACE: set remote state ret=%d\n", ret);

	ret = iris_load_fw_to_memory(core, fwpath);
	if (ret) {
		dev_err(core->dev, "firmware download failed\n");
		return -ENOMEM;
	}

	if (iris_no_auth) {
		dev_err(core->dev, "IRIS-TRACE: no_auth: image init done, stopping\n");
		dev_err(core->dev,
			"IRIS-TRACE: WINDOW OPEN 15 s - run: sudo python3 tools/videocc-peek.py dump\n");
		msleep(15000);
		dev_err(core->dev, "IRIS-TRACE: window closed\n");
		return -EIO;
	}

	dev_err(core->dev, "IRIS-TRACE: PAS auth and reset (SCM call)\n");
	ret = qcom_scm_pas_auth_and_reset(iris_pas_id(core));
	dev_err(core->dev, "IRIS-TRACE: PAS auth ret=%d\n", ret);
	if (ret)  {
		dev_err(core->dev, "auth and reset failed: %d\n", ret);
		return ret;
	}

	dev_err(core->dev, "IRIS-TRACE: mem protect video var (SCM call)\n");
	ret = qcom_scm_mem_protect_video_var(cp_config->cp_start,
					     cp_config->cp_size,
					     cp_config->cp_nonpixel_start,
					     cp_config->cp_nonpixel_size);
	if (ret) {
		dev_err(core->dev, "protect memory failed\n");
		qcom_scm_pas_shutdown(iris_pas_id(core));
		return ret;
	}

	return ret;
}

int iris_fw_unload(struct iris_core *core)
{
	return qcom_scm_pas_shutdown(iris_pas_id(core));
}

int iris_set_hw_state(struct iris_core *core, bool resume)
{
	return qcom_scm_set_remote_state(resume, 0);
}
