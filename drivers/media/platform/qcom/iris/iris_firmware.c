// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/arm-smccc.h>
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

/*
 * DEBUG: bisect "the firmware alone wedges the machine" against "our first
 * register access after the core is released does".  With this set, nothing
 * touches the VPU for half the interval right after qcom_scm_pas_auth_and_reset()
 * succeeds; then one register snapshot; then the other half.  If the log never
 * reaches "hold 1/2 survived" the core's own execution is what takes the bus
 * down and no amount of driver-side register work will help.
 */
static unsigned int iris_hold_after_auth_ms;
module_param_named(hold_after_auth_ms, iris_hold_after_auth_ms, uint, 0644);
MODULE_PARM_DESC(hold_after_auth_ms, "IRIS debug: after PAS auth, wait this long touching no VPU register");

/*
 * DEBUG: the two secure-memory calls the Windows PIL makes that this driver
 * does not.
 *
 * qcpil8180.sys builds them as SIP/PIL SCM calls - the same wire format as
 * mainline's struct qcom_scm_desc - and its load sequence is:
 *
 *	PIL/6  "unlock subsystem memory" (TreeDirectPerformUnlockXpu), arg = pas id
 *	PIL/1  init_image       } both done by qcom_mdt_load() here
 *	PIL/2  mem_setup        }
 *	MP/0x16 assign_mem      -> mainline qcom_scm_assign_mem()
 *	PIL/0xb "share subsystem memory" (no mainline equivalent)
 *
 * PIL/6 is the same SMC id mainline wraps as qcom_scm_pas_shutdown(), and
 * qcpil's WPP message for it is the one that reads "Request to unlock subsystem
 * memory, TZ processor ID: %d".  It is issued before init_image, i.e. before
 * anything of ours has started the core, which is why trying it is cheap.
 *
 * The assign is the interesting one: mainline's remoteproc PAS path hands the
 * loaded firmware region to its subsystem's VMID before start (see
 * qcom_q6v5_pas.c's region_assign_shared idiom), and the video has no VMID of
 * its own - its secure contexts are the CP_* ones (CP_BITSTREAM 0x9,
 * CP_PIXEL 0xa, CP_NON_PIXEL 0xb), which is exactly the split the vendor's
 * sc8180x DTS describes.  Nothing in iris ever assigns anything, so the
 * firmware's memory is still HLOS-owned when the core is released.
 */
static unsigned int iris_assign_vmid;
module_param_named(assign_vmid, iris_assign_vmid, uint, 0644);
MODULE_PARM_DESC(assign_vmid, "IRIS debug: also assign the firmware and UC regions to this VMID, keeping HLOS (0 = off)");

/*
 * DEBUG: PIL/6 is deliberately NOT here.  qcpil8180.sys labels SIP/PIL/6
 * "unlock subsystem memory" and mainline wraps it as qcom_scm_pas_shutdown(),
 * so issuing it before the firmware load looked free.  It is not: on this TZ
 * the call never returns when the subsystem has not been started (the probe
 * thread parks in the SMC, the GPU's own TZ traffic starves and the display
 * locks up).  Windows only sends it behind a guard (obj[0x110] == 1, cleared
 * afterwards), which is consistent with mainline's naming.
 *
 * The assign below can be placed in one of two spots instead:
 *   assign_late = 0 -> right after qcom_mdt_load() (image in memory, before auth)
 *   assign_late = 1 -> right after qcom_scm_mem_protect_video_var() (MP/8, the
 *                      video CP windows are configured, still before the core is
 *                      kicked by CTRL_INIT)
 */
static unsigned int iris_assign_late;
module_param_named(assign_late, iris_assign_late, uint, 0644);
MODULE_PARM_DESC(assign_late, "IRIS debug: place the assign after mem_protect_video_var instead of after the image load");

static unsigned int iris_stop_before_boot;
module_param_named(stop_before_boot, iris_stop_before_boot, uint, 0644);
MODULE_PARM_DESC(stop_before_boot, "IRIS debug: with the core released but before CTRL_INIT, stop (safe)");

/*
 * DEBUG: PIL/0xb "share subsystem memory" - the last step of the Windows PIL
 * sequence, and the only one of its calls that the Windows VTL1 SK extension
 * does not intercept (its SMC dispatch table has no case for 0x0200020b), so it
 * reaches the real TrustZone.  mainline has no wrapper for it, hence
 * qcom_scm_debug_call().
 *
 * Request (qcpil8180.sys fn 0x14000cd90): id 0x0200020b, arginfo 4, args
 * { in0, in4, (s32)in8, 3 = QCOM_SCM_VMID_HLOS }, where in[] is a 12-byte user
 * mode block.  in8 is the TZ processor id (the PAS id).  in0/in4 are either
 * (address, size) or (address_lo, address_hi); both readings survive the
 * reverse engineering, so both are selectable here.
 */
static unsigned int iris_pil0b_share;
module_param_named(pil0b_share, iris_pil0b_share, uint, 0644);
MODULE_PARM_DESC(pil0b_share, "IRIS debug: issue PIL/0xb for the UC region (1 = args addr/size, 2 = args addr_lo/addr_hi)");

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

	if (!ret && iris_assign_vmid && !iris_assign_late) {
		struct qcom_scm_vmperm perm[2] = {
			{ .vmid = QCOM_SCM_VMID_HLOS, .perm = QCOM_SCM_PERM_RW },
			{ .vmid = iris_assign_vmid, .perm = QCOM_SCM_PERM_RW },
		};
		phys_addr_t uc_phys = virt_to_phys(core->iface_q_table_vaddr);
		u64 owners;

		owners = BIT(QCOM_SCM_VMID_HLOS);
		ret = qcom_scm_assign_mem(mem_phys, res_size, &owners, perm, 2);
		dev_err(dev,
			"IRIS-TRACE: assign fw region %pa size %zu to vmid %#x: ret=%d owners=%#llx\n",
			&mem_phys, res_size, iris_assign_vmid, ret, owners);

		owners = BIT(QCOM_SCM_VMID_HLOS);
		ret = qcom_scm_assign_mem(uc_phys, core->uc_region_size,
					  &owners, perm, 2);
		dev_err(dev,
			"IRIS-TRACE: assign uc region %pa (iova %#llx) size %u to vmid %#x: ret=%d owners=%#llx\n",
			&uc_phys, (u64)core->iface_q_table_daddr,
			core->uc_region_size, iris_assign_vmid, ret, owners);

		ret = 0;	/* the assign result is logged, not acted on */
	}

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

	if (iris_hold_after_auth_ms) {
		unsigned int half = iris_hold_after_auth_ms / 2;

		dev_err(core->dev,
			"IRIS-TRACE: hold: %u ms with no VPU register access at all\n",
			iris_hold_after_auth_ms);
		msleep(half);
		dev_err(core->dev, "IRIS-TRACE: hold 1/2 survived, first register snapshot now\n");
		iris_vpu_trace_isr(core, "hold-read");
		msleep(half);
		dev_err(core->dev, "IRIS-TRACE: hold 2/2 survived, continuing\n");
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

	if (iris_assign_vmid && iris_assign_late) {
		struct qcom_scm_vmperm perm[2] = {
			{ .vmid = QCOM_SCM_VMID_HLOS, .perm = QCOM_SCM_PERM_RW },
			{ .vmid = iris_assign_vmid, .perm = QCOM_SCM_PERM_RW },
		};
		phys_addr_t uc_phys = virt_to_phys(core->iface_q_table_vaddr);
		u64 owners = BIT(QCOM_SCM_VMID_HLOS);
		int aret;

		aret = qcom_scm_assign_mem(uc_phys, core->uc_region_size,
					   &owners, perm, 2);
		dev_err(core->dev,
			"IRIS-TRACE: late assign uc region %pa (iova %#llx) size %u to vmid %#x: ret=%d owners=%#llx\n",
			&uc_phys, (u64)core->iface_q_table_daddr,
			core->uc_region_size, iris_assign_vmid, aret, owners);
	}

	if (iris_pil0b_share) {
		u32 pas_id = iris_pas_id(core);
		phys_addr_t uc_phys = virt_to_phys(core->iface_q_table_vaddr);
		u64 args[4], sres[3] = {};
		int sret;

		if (iris_pil0b_share == 2) {
			/* reading R1: (address_lo, address_hi) */
			args[0] = (u32)uc_phys;
			args[1] = (u64)uc_phys >> 32;
		} else {
			/* reading R2: (address, size) */
			args[0] = uc_phys;
			args[1] = core->uc_region_size;
		}
		args[2] = pas_id;
		args[3] = QCOM_SCM_VMID_HLOS;

		sret = qcom_scm_debug_call(0x02, 0x0b, ARM_SMCCC_OWNER_SIP, 4,
					   args, 4, sres);
		dev_err(core->dev,
			"IRIS-TRACE: PIL/0xb share subsystem memory (%s) uc %pa size %u pas %u: ret=%d res=%#llx %#llx %#llx\n",
			iris_pil0b_share == 2 ? "addr_lo/hi" : "addr/size",
			&uc_phys, core->uc_region_size, pas_id, sret,
			sres[0], sres[1], sres[2]);
	}

	if (iris_stop_before_boot) {
		/*
		 * Safe place to stop: the core is released but never kicked -
		 * CTRL_INIT is what starts it - and -15 showed that a released,
		 * idle core sits there indefinitely without harming anything.
		 */
		dev_err(core->dev,
			"IRIS-TRACE: stop before CTRL_INIT (core released, never kicked)\n");
		return -EIO;
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
