// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/pm_runtime.h>

#include "iris_core.h"
#include "iris_firmware.h"
#include "iris_state.h"
#include "iris_vpu_common.h"

void iris_core_deinit(struct iris_core *core)
{
	pm_runtime_resume_and_get(core->dev);

	mutex_lock(&core->lock);
	if (core->state != IRIS_CORE_DEINIT) {
		iris_fw_unload(core);
		iris_vpu_power_off(core);
		iris_hfi_queues_deinit(core);
		core->state = IRIS_CORE_DEINIT;
	}
	mutex_unlock(&core->lock);

	pm_runtime_put_sync(core->dev);
}

static int iris_wait_for_system_response(struct iris_core *core)
{
	u32 hw_response_timeout_val = core->iris_platform_data->hw_response_timeout;
	int ret;

	if (core->state == IRIS_CORE_ERROR)
		return -EIO;

	ret = wait_for_completion_timeout(&core->core_init_done,
					  msecs_to_jiffies(hw_response_timeout_val));
	if (!ret) {
		core->state = IRIS_CORE_ERROR;
		return -ETIMEDOUT;
	}

	return 0;
}

int iris_core_init(struct iris_core *core)
{
	int ret;

	mutex_lock(&core->lock);
	if (core->state == IRIS_CORE_INIT) {
		ret = 0;
		goto exit;
	} else if (core->state == IRIS_CORE_ERROR) {
		ret = -EINVAL;
		goto error;
	}

	core->state = IRIS_CORE_INIT;

	dev_err(core->dev, "IRIS-TRACE: core_init: hfi queues [dbg=intclear1]\n");
	ret = iris_hfi_queues_init(core);
	if (ret)
		goto error;

	dev_err(core->dev, "IRIS-TRACE: core_init: vpu power_on\n");
	ret = iris_vpu_power_on(core);
	if (ret)
		goto error_queue_deinit;

	dev_err(core->dev, "IRIS-TRACE: core_init: firmware load\n");
	ret = iris_fw_load(core);
	if (ret)
		goto error_power_off;

	dev_err(core->dev, "IRIS-TRACE: core_init: boot firmware\n");
	ret = iris_vpu_boot_firmware(core);
	if (ret)
		goto error_unload_fw;

	dev_err(core->dev, "IRIS-TRACE: core_init: hfi core init\n");
	ret = iris_hfi_core_init(core);
	if (ret)
		goto error_unload_fw;

	mutex_unlock(&core->lock);

	dev_err(core->dev, "IRIS-TRACE: core_init: waiting for sys response\n");
	return iris_wait_for_system_response(core);

error_unload_fw:
	iris_fw_unload(core);
error_power_off:
	iris_vpu_power_off(core);
error_queue_deinit:
	iris_hfi_queues_deinit(core);
error:
	core->state = IRIS_CORE_DEINIT;
exit:
	mutex_unlock(&core->lock);

	return ret;
}
