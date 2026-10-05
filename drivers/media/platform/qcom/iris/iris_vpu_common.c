// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_opp.h>
#include <linux/reset.h>

#include "iris_core.h"
#include "iris_vpu_common.h"
#include "iris_vpu_register_defines.h"

#define WRAPPER_TZ_BASE_OFFS			0x000C0000
#define AON_BASE_OFFS				0x000E0000

/*
 * TEST: the CPU interrupt controller.  The Venus-4xx map this SoC uses puts it
 * at CPU_BASE + 0x1F000, with the H2X doorbell at +0x18 and its bit at
 * BIT(15) - both the vendor's sdmshrike header (VIDC_CPU_IC_BASE_OFFS,
 * VIDC_CPU_IC_SOFTINT_H2A_SHFT 0xF) and mainline venus agree
 * (hfi_venus_io.h: CPU_IC_BASE = CPU_BASE + 0x1f000, CPU_IC_SOFTINT 0x18,
 * CPU_IC_SOFTINT_H2A_SHIFT 0xf; venus_soft_int() picks those unless
 * IS_V6()/AR50-lite).
 *
 * What was here were the V6 values - CPU_IC_BASE == CPU_BASE and offset 0x150,
 * bit 0 - so every H2X doorbell this driver ever raised went to 0xC0150 with
 * value 1 instead of to 0xDF018 with value 0x8000: the firmware was never told
 * that a command had arrived.  That is why it never drained the command queue
 * and never answered SYS_INIT (msgq_write_idx stayed 0), and the stray write
 * into the middle of the CPU block is a candidate for the hang that follows it.
 */
#define CPU_IC_BASE_OFFS			(CPU_BASE_OFFS + 0x1F000)

#define CPU_CS_A2HSOFTINTCLR			(CPU_CS_BASE_OFFS + 0x1C)
#define CLEAR_XTENSA2HOST_INTR			BIT(0)

#define CTRL_INIT				(CPU_CS_BASE_OFFS + 0x48)
#define CTRL_STATUS				(CPU_CS_BASE_OFFS + 0x4C)

#define CTRL_INIT_IDLE_MSG_BMSK			0x40000000
#define CTRL_ERROR_STATUS__M			0xfe
#define CTRL_STATUS_PC_READY			0x100

#define QTBL_INFO				(CPU_CS_BASE_OFFS + 0x50)
#define QTBL_ENABLE				BIT(0)

#define QTBL_ADDR				(CPU_CS_BASE_OFFS + 0x54)
#define CPU_CS_SCIACMDARG3			(CPU_CS_BASE_OFFS + 0x58)
#define SFR_ADDR				(CPU_CS_BASE_OFFS + 0x5C)
#define UC_REGION_ADDR				(CPU_CS_BASE_OFFS + 0x64)
#define UC_REGION_SIZE				(CPU_CS_BASE_OFFS + 0x68)

#define CPU_CS_H2XSOFTINTEN			(CPU_CS_BASE_OFFS + 0x148)
#define HOST2XTENSA_INTR_ENABLE			BIT(0)

#define CPU_CS_X2RPMH				(CPU_CS_BASE_OFFS + 0x168)
#define MSK_SIGNAL_FROM_TENSILICA		BIT(0)
#define MSK_CORE_POWER_ON			BIT(1)

/* 4xx map: SOFTINT at CPU_IC + 0x18, H2X bit 15 (V6: +0x150, bit 0). */
#define CPU_IC_SOFTINT				(CPU_IC_BASE_OFFS + 0x18)
#define CPU_IC_SOFTINT_H2A_SHFT			0xF

#define WRAPPER_INTR_STATUS			(WRAPPER_BASE_OFFS + 0x0C)
#define WRAPPER_INTR_STATUS_A2H_BMSK		BIT(2)

#define WRAPPER_INTR_MASK			(WRAPPER_BASE_OFFS + 0x10)
#define WRAPPER_INTR_CLEAR			(WRAPPER_BASE_OFFS + 0x14)
#define WRAPPER_INTR_MASK_A2HCPU_BMSK		BIT(2)

/*
 * TEST: the A2H watchdog bit.  The Venus-4xx map this SoC uses calls BIT(3)
 * A2HVCODEC and puts the watchdog on BIT(4); the V6/IRIS2 map reuses BIT(3)
 * for the watchdog (venus: WRAPPER_INTR_MASK_A2HWD_BASK_V6), which is where
 * the BIT(3) the driver had came from.  With BIT(3) the read-modify-write in
 * iris_vpu_interrupt_init() produced 0x1f2 - the "Venus-6xx mask" that
 * iris_vpu_boot_firmware() then tried to correct by writing 0x8, which is the
 * vpu4/SDM845 value.  The vendor's vpu5 path (sdmshrike, msm-4.14
 * interrupt_init_vpu5()) clears only the CPU and watchdog bits and leaves
 * 0x1e2.  Same caveat as the CPU/WRAPPER base offsets in
 * iris_vpu_register_defines.h: this is shared with the V6 platforms and has
 * to become a per-platform value before anything upstream.
 */
#define WRAPPER_INTR_STATUS_A2HWD_BMSK		BIT(4)
#define WRAPPER_INTR_MASK_A2HWD_BMSK		BIT(4)

/*
 * Frequency the core is brought up at before any instance has voted for a
 * rate.  200 MHz is what the vendor PIL asks for (sdmshrike.dtsi,
 * qcom,core-freq = qcom,ahb-freq = <200000000>).  The OPP core rounds it to
 * what the clock can actually produce - 200000097 Hz on this hardware - and
 * uses the next OPP up, 240 MHz, only for its required-opps voltage corner:
 * _opp_config_clk_single() programs *target for the rate, not opp->rate.  The
 * MVS0, MVSC and AHB branches all hang off video_cc_iris_clk_src, so the one
 * set_rate covers the core and the AHB interface clock together.
 */
#define IRIS_BOOT_FREQ				200000000

/*
 * DEBUG: bisect "CTRL_INIT alone is enough to kill it" against "the HFI
 * command that follows is".  With this set the driver waits, touching nothing,
 * between CTRL_INIT being acknowledged and the first HFI command.
 */
static unsigned int iris_hold_after_ctrl_init_ms;
module_param_named(hold_after_ctrl_init_ms, iris_hold_after_ctrl_init_ms, uint, 0644);
MODULE_PARM_DESC(hold_after_ctrl_init_ms, "IRIS debug: after CTRL_INIT is acked, wait this long before any HFI command");

/*
 * DEBUG: the same bisect one step earlier, and the one that does not need the
 * machine to survive a wedged bus.  Every run so far has died somewhere within
 * ~10 ms of the CTRL_INIT write, and in two of six attempts it died before the
 * poll loop's next CTRL_STATUS read could print - i.e. a CPU was very likely
 * stuck inside that read when the VPU's bus went away.  With this set the
 * driver writes CTRL_INIT and then touches *no* VPU register at all: it sleeps,
 * reads the SFR (plain DDR, so it still works with the VPU bus gone) and fails
 * the probe cleanly.  Whatever the firmware has to say about CTRL_INIT gets
 * printed, and the machine stays alive to print it.
 */
static unsigned int iris_quiet_after_ctrl_init_ms;
module_param_named(quiet_after_ctrl_init_ms, iris_quiet_after_ctrl_init_ms, uint, 0644);
MODULE_PARM_DESC(quiet_after_ctrl_init_ms, "IRIS debug: after the CTRL_INIT write, read no VPU register for this long, then only the SFR");

static void iris_vpu_trace_sfr(struct iris_core *core, const char *tag);

#define WRAPPER_DEBUG_BRIDGE_LPI_CONTROL	(WRAPPER_BASE_OFFS + 0x54)
#define WRAPPER_DEBUG_BRIDGE_LPI_STATUS		(WRAPPER_BASE_OFFS + 0x58)
#define WRAPPER_IRIS_CPU_NOC_LPI_CONTROL	(WRAPPER_BASE_OFFS + 0x5C)
#define WRAPPER_IRIS_CPU_NOC_LPI_STATUS		(WRAPPER_BASE_OFFS + 0x60)

#define WRAPPER_TZ_CPU_STATUS			(WRAPPER_TZ_BASE_OFFS + 0x10)
#define WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG		(WRAPPER_TZ_BASE_OFFS + 0x14)
#define CTL_AXI_CLK_HALT			BIT(0)
#define CTL_CLK_HALT				BIT(1)

#define WRAPPER_TZ_QNS4PDXFIFO_RESET		(WRAPPER_TZ_BASE_OFFS + 0x18)
#define RESET_HIGH				BIT(0)

#define AON_WRAPPER_MVP_NOC_LPI_CONTROL		(AON_BASE_OFFS)
#define REQ_POWER_DOWN_PREP			BIT(0)

#define AON_WRAPPER_MVP_NOC_LPI_STATUS		(AON_BASE_OFFS + 0x4)

static void iris_vpu_interrupt_init(struct iris_core *core)
{
	u32 mask_val;

	mask_val = readl(core->reg_base + WRAPPER_INTR_MASK);
	dev_err(core->dev, "IRIS-TRACE: WRAPPER_INTR_MASK (+0x%x) = 0x%x (expect 0x1f6)\n",
		WRAPPER_INTR_MASK, mask_val);
	mask_val &= ~(WRAPPER_INTR_MASK_A2HWD_BMSK |
		      WRAPPER_INTR_MASK_A2HCPU_BMSK);
	writel(mask_val, core->reg_base + WRAPPER_INTR_MASK);
}

static void iris_vpu_setup_ucregion_memory_map(struct iris_core *core)
{
	u32 value;
	const struct vpu_ops *vpu_ops = core->iris_platform_data->vpu_ops;

	value = (u32)core->iface_q_table_daddr;
	writel(value, core->reg_base + UC_REGION_ADDR);

	/*
	 * This has to be the size iris_hfi_queues_init() actually allocated at
	 * that address.  It used to be recomputed here as
	 * ALIGN(SFR_SIZE + queue_size, SZ_1M) while the queue table allocation
	 * was only queue_size, so the firmware was told its UC region extended
	 * ~0.5 MB into unmapped IOVA.  The vendor driver for this SoC
	 * advertises exactly what it allocated.
	 */
	writel(core->uc_region_size, core->reg_base + UC_REGION_SIZE);

	value = (u32)core->iface_q_table_daddr;
	writel(value, core->reg_base + QTBL_ADDR);

	writel(QTBL_ENABLE, core->reg_base + QTBL_INFO);

	if (core->sfr_daddr) {
		value = (u32)core->sfr_daddr + core->iris_platform_data->core_arch;
		writel(value, core->reg_base + SFR_ADDR);
	}

	if (vpu_ops->program_bootup_registers)
		vpu_ops->program_bootup_registers(core);

	dev_err(core->dev,
		"IRIS-TRACE: ucregion: qtable=%#x size=%#x sfr=%#x\n",
		(u32)core->iface_q_table_daddr, core->uc_region_size,
		(u32)core->sfr_daddr);
}

/*
 * DEBUG: one-shot read of the registers that decide how the next few
 * milliseconds go.  WRAPPER_HW_VERSION identifies the core revision (the
 * firmware string says VIDEO.IR.1.2 while the binding calls the sm8250 core
 * "Iris v2.xx"); the VCODEC NoC error block says whether the VPU has already
 * logged a NoC fault, which is the shape of the "reset from below Linux" this
 * probe ends on.  Both are inside the node's window and are read once per core
 * init.
 */
static void iris_vpu_trace_probe_registers(struct iris_core *core)
{
	u32 ver, errvld, errlog0, errlog1;

	ver = readl(core->reg_base + WRAPPER_HW_VERSION);
	errvld = readl(core->reg_base + VCODEC_CORE0_VIDEO_NOC_BASE_OFFS +
		       VCODEC_NOC_ERR_ERRVLD_LOW_OFFS);

	dev_err(core->dev,
		"IRIS-TRACE: probe: hw_version=%#x (major %u minor %u step %#x) intr_mask=%#x noc_errvld=%#x\n",
		ver,
		(ver & WRAPPER_HW_VERSION_MAJOR_MASK) >> WRAPPER_HW_VERSION_MAJOR_SHIFT,
		(ver & WRAPPER_HW_VERSION_MINOR_MASK) >> WRAPPER_HW_VERSION_MINOR_SHIFT,
		ver & WRAPPER_HW_VERSION_STEP_MASK,
		readl(core->reg_base + WRAPPER_INTR_MASK), errvld);

	/*
	 * Only descend into the log words when the sticky bit is set: they are
	 * two more reads of a block this board has not been talked to yet.
	 */
	if (errvld) {
		errlog0 = readl(core->reg_base + VCODEC_CORE0_VIDEO_NOC_BASE_OFFS +
				VCODEC_NOC_ERR_ERRLOG0_LOW_OFFS);
		errlog1 = readl(core->reg_base + VCODEC_CORE0_VIDEO_NOC_BASE_OFFS +
				VCODEC_NOC_ERR_ERRLOG0_HIGH_OFFS);
		dev_err(core->dev,
			"IRIS-TRACE: probe: noc_errlog0=%#x/%#x (ack via ERRCLR at +%#x)\n",
			errlog0, errlog1, VCODEC_NOC_ERR_ERRCLR_LOW_OFFS);
	}
}

int iris_vpu_boot_firmware(struct iris_core *core)
{
	u32 ctrl_init = BIT(0), ctrl_status = 0, count = 0, max_tries = 1000;

	dev_err(core->dev, "IRIS-TRACE: boot_fw: probe registers\n");
	iris_vpu_trace_probe_registers(core);

	dev_err(core->dev, "IRIS-TRACE: boot_fw: ucregion map\n");
	iris_vpu_setup_ucregion_memory_map(core);

	/*
	 * WRAPPER_INTR_MASK was already programmed by iris_vpu_interrupt_init()
	 * (see the A2HWD note above): a read-modify-write that clears only the
	 * CPU and watchdog bits, from the 0x1f6 reset value to 0x1e2, which is
	 * what the vendor's vpu5 path does.  hfi_venus.c's AR50 branch writes
	 * 0x8 here instead, but that is the vpu4/SDM845 value - it masks the
	 * VCODEC source and unmasks the extra level sources the vendor leaves
	 * masked.  Do not narrow it again; the value is printed by the trace
	 * above so the first boot shows which one the core got.
	 */

	/*
	 * CPU_CS_SCIACMDARG3 is VIDC_VERSION_INFO.  hfi_venus.c writes 1 there
	 * only for IS_V1() cores and the vendor's vpu5 path for this SoC never
	 * writes it at all - so writing it here advertised HFI 1.x to a
	 * firmware that is being fed HFI-4xx packets.  Removed; if the firmware
	 * ever turns out to need it, it is a one-line experiment.
	 */
	dev_err(core->dev, "IRIS-TRACE: boot_fw: CTRL_INIT write\n");
	writel(ctrl_init, core->reg_base + CTRL_INIT);

	if (iris_quiet_after_ctrl_init_ms) {
		/*
		 * No VPU register access from here on - that is the point.  If
		 * the firmware has written a reason into the SFR it will come
		 * out even when the VPU's own bus is gone, and if it is the
		 * polling that wedges the machine, this is what keeps it alive
		 * to say so.
		 */
		dev_err(core->dev,
			"IRIS-TRACE: quiet: %u ms after the CTRL_INIT write, no register access\n",
			iris_quiet_after_ctrl_init_ms);
		msleep(iris_quiet_after_ctrl_init_ms);
		iris_vpu_trace_sfr(core, "quiet");
		dev_err(core->dev,
			"IRIS-TRACE: quiet: done, no VPU register was read after CTRL_INIT\n");
		return -ETIME;
	}

	while (!ctrl_status && count < max_tries) {
		ctrl_status = readl(core->reg_base + CTRL_STATUS);
		if ((ctrl_status & CTRL_ERROR_STATUS__M) == 0x4) {
			dev_err(core->dev, "invalid setting for uc_region\n");
			break;
		}

		usleep_range(50, 100);
		count++;
	}

	dev_err(core->dev, "IRIS-TRACE: boot_fw: ctrl_status=%#x count=%u\n", ctrl_status, count);
	if (count >= max_tries) {
		dev_err(core->dev, "error booting up iris firmware\n");
		return -ETIME;
	}

	/* CPU_CS_H2XSOFTINTEN (0x148) and CPU_CS_X2RPMH (0x168) are Venus-6xx
	 * registers; hfi_venus.c writes them only for IRIS2/IRIS2_1/AR50-lite,
	 * and the Venus-4xx boot path stops after CTRL_INIT. */

	/* What did the firmware make of CTRL_INIT?  Its error channel first. */
	iris_vpu_trace_sfr(core, "post-ctrl-init");

	if (iris_hold_after_ctrl_init_ms) {
		dev_err(core->dev,
			"IRIS-TRACE: hold: %u ms after CTRL_INIT, before any HFI command\n",
			iris_hold_after_ctrl_init_ms);
		msleep(iris_hold_after_ctrl_init_ms);
		dev_err(core->dev, "IRIS-TRACE: hold after CTRL_INIT survived\n");
		iris_vpu_trace_isr(core, "hold-ctrl-init");
	}

	return 0;
}

/*
 * DEBUG: wait for the firmware's SYS_INIT response by polling instead of
 * sleeping on the completion, so that a machine that dies inside this window
 * leaves the last readable state on the console.  The registers are ones this
 * driver already touches: CPU_CS CTRL_STATUS, the wrapper's INTR_STATUS, the
 * VCODEC NoC error-valid bit (a NoC error is what a "reset from below Linux"
 * looks like) and the message queue's write index - i.e. whether the firmware
 * ever wrote a response at all.  Only changes are printed, plus the total on
 * success or timeout.
 */
int iris_vpu_wait_for_core_init(struct iris_core *core, u32 timeout_ms)
{
	struct iris_hfi_queue_header *cq = core->command_queue.qhdr;
	struct iris_hfi_queue_header *mq = core->message_queue.qhdr;
	struct iris_hfi_queue_header *dq = core->debug_queue.qhdr;
	unsigned long start = jiffies;
	unsigned long deadline = start + msecs_to_jiffies(timeout_ms);
	u32 intr, ctrl, noc, cr, mw, dw;
	u32 p_intr = ~0, p_ctrl = ~0, p_noc = ~0, p_cr = ~0, p_mw = ~0, p_dw = ~0;

	for (;;) {
		if (try_wait_for_completion(&core->core_init_done)) {
			dev_err(core->dev, "IRIS-TRACE: wait: response after %u ms\n",
				jiffies_to_msecs(jiffies - start));
			return 0;
		}

		intr = readl(core->reg_base + WRAPPER_INTR_STATUS);
		ctrl = readl(core->reg_base + CTRL_STATUS);
		noc = readl(core->reg_base + VCODEC_CORE0_VIDEO_NOC_BASE_OFFS +
			    VCODEC_NOC_ERR_ERRVLD_LOW_OFFS);
		cr = cq ? cq->read_idx : 0;
		mw = mq ? mq->write_idx : 0;
		dw = dq ? dq->write_idx : 0;

		if (intr != p_intr || ctrl != p_ctrl || noc != p_noc ||
		    cr != p_cr || mw != p_mw || dw != p_dw) {
			dev_err(core->dev,
				"IRIS-TRACE: wait t=%ums intr_status=%#x ctrl_status=%#x noc_errvld=%#x cmdq_r=%u msgq_w=%u dbgq_w=%u\n",
				jiffies_to_msecs(jiffies - start), intr, ctrl, noc,
				cr, mw, dw);
			/* the firmware's own error channel, plain DDR */
			iris_vpu_trace_sfr(core, "wait");
			p_intr = intr;
			p_ctrl = ctrl;
			p_noc = noc;
			p_cr = cr;
			p_mw = mw;
			p_dw = dw;
		}

		if (time_after(jiffies, deadline))
			break;

		udelay(50);
	}

	dev_err(core->dev, "IRIS-TRACE: wait: no response in %u ms\n", timeout_ms);

	return -ETIMEDOUT;
}

void iris_vpu_raise_interrupt(struct iris_core *core)
{
	writel(1 << CPU_IC_SOFTINT_H2A_SHFT, core->reg_base + CPU_IC_SOFTINT);
}


void iris_vpu_clear_interrupt(struct iris_core *core)
{
	u32 intr_status, mask;

	intr_status = readl(core->reg_base + WRAPPER_INTR_STATUS);
	mask = (WRAPPER_INTR_STATUS_A2H_BMSK |
		WRAPPER_INTR_STATUS_A2HWD_BMSK |
		CTRL_INIT_IDLE_MSG_BMSK);

	if (intr_status & mask)
		core->intr_status |= intr_status;

	writel(CLEAR_XTENSA2HOST_INTR, core->reg_base + CPU_CS_A2HSOFTINTCLR);

	/* The wrapper keeps its own status latch; the IRQ is level triggered, so
	 * on the older cores (everything that is not IRIS2/AR50-lite, which is
	 * what this SoC is) it has to be cleared here too or the interrupt
	 * re-fires forever.  hfi_venus.c does the same. */
	if (intr_status)
		writel(intr_status, core->reg_base + WRAPPER_INTR_CLEAR);
}

/*
 * DEBUG: the SFR (subsystem failure reason) is the firmware's own error
 * channel - it writes a reason string there when it decides it cannot go on,
 * and raises A2H.  That is exactly the state the probe keeps ending in (A2H
 * set, all three queues untouched), and the SFR is plain DDR: reading it costs
 * no VPU register access, so it is safe to look at even while the machine is
 * about to wedge.
 */
static void iris_vpu_trace_sfr(struct iris_core *core, const char *tag)
{
	u32 size, written, n, hn, i;
	u8 *data = core->sfr_vaddr;
	char buf[161], hex[3 * 25];

	if (!data)
		return;

	size = ((u32 *)data)[0];
	written = ((u32 *)data)[1];
	if (written > SFR_SIZE - 8)
		written = SFR_SIZE - 8;

	n = min_t(u32, written, sizeof(buf) - 1);
	for (i = 0; i < n; i++) {
		u8 c = data[8 + i];

		buf[i] = (c >= 32 && c < 127) ? c : '.';
	}
	buf[n] = '\0';

	hn = min_t(u32, written, 24);
	for (i = 0; i < hn; i++)
		snprintf(hex + 3 * i, 4, "%02x ", data[8 + i]);
	if (hn)
		hex[3 * hn - 1] = '\0';
	else
		hex[0] = '\0';

	dev_err(core->dev,
		"IRIS-TRACE: sfr[%s]: size=%u written=%u text='%s' hex=%s\n",
		tag, size, written, buf, hex);
}

/*
 * DEBUG: snapshot of what the IRQ path sees.  Capped, because the most likely
 * shape of the hang this is chasing is an interrupt storm - the wrapper's A2H
 * line is level-based on "the firmware has something in the message queue", so
 * it stays asserted until the queue is drained, and a driver that never drains
 * it will re-enter here forever.  A capped print shows the first few rounds
 * (with read/write indices, which is what says whether anything was drained)
 * without turning the console into the second problem.
 */
void iris_vpu_trace_isr(struct iris_core *core, const char *tag)
{
	struct iris_hfi_queue_header *cq = core->command_queue.qhdr;
	struct iris_hfi_queue_header *mq = core->message_queue.qhdr;
	struct iris_hfi_queue_header *dq = core->debug_queue.qhdr;
	static u32 count;

	if (count++ > 24)
		return;

	dev_err(core->dev,
		"IRIS-TRACE: isr[%u %s]: intr_status=%#x intr_mask=%#x ctrl_status=%#x noc_errvld=%#x cmdq_rw=%u/%u msgq_rw=%u/%u dbgq_rw=%u/%u\n",
		count - 1, tag,
		readl(core->reg_base + WRAPPER_INTR_STATUS),
		readl(core->reg_base + WRAPPER_INTR_MASK),
		readl(core->reg_base + CTRL_STATUS),
		readl(core->reg_base + VCODEC_CORE0_VIDEO_NOC_BASE_OFFS +
		       VCODEC_NOC_ERR_ERRVLD_LOW_OFFS),
		cq ? cq->read_idx : 0, cq ? cq->write_idx : 0,
		mq ? mq->read_idx : 0, mq ? mq->write_idx : 0,
		dq ? dq->read_idx : 0, dq ? dq->write_idx : 0);

	iris_vpu_trace_sfr(core, tag);
}

int iris_vpu_watchdog(struct iris_core *core, u32 intr_status)
{
	if (intr_status & WRAPPER_INTR_STATUS_A2HWD_BMSK) {
		dev_err(core->dev, "received watchdog interrupt\n");
		return -ETIME;
	}

	return 0;
}

int iris_vpu_prepare_pc(struct iris_core *core)
{
	u32 wfi_status, idle_status, pc_ready;
	u32 ctrl_status, val = 0;
	int ret;

	ctrl_status = readl(core->reg_base + CTRL_STATUS);
	pc_ready = ctrl_status & CTRL_STATUS_PC_READY;
	idle_status = ctrl_status & BIT(30);
	if (pc_ready)
		return 0;

	wfi_status = readl(core->reg_base + WRAPPER_TZ_CPU_STATUS);
	wfi_status &= BIT(0);
	if (!wfi_status || !idle_status)
		goto skip_power_off;

	ret = core->hfi_ops->sys_pc_prep(core);
	if (ret)
		goto skip_power_off;

	ret = readl_poll_timeout(core->reg_base + CTRL_STATUS, val,
				 val & CTRL_STATUS_PC_READY, 250, 2500);
	if (ret)
		goto skip_power_off;

	ret = readl_poll_timeout(core->reg_base + WRAPPER_TZ_CPU_STATUS,
				 val, val & BIT(0), 250, 2500);
	if (ret)
		goto skip_power_off;

	return 0;

skip_power_off:
	ctrl_status = readl(core->reg_base + CTRL_STATUS);
	wfi_status = readl(core->reg_base + WRAPPER_TZ_CPU_STATUS);
	wfi_status &= BIT(0);
	dev_err(core->dev, "skip power collapse, wfi=%#x, idle=%#x, pcr=%#x, ctrl=%#x)\n",
		wfi_status, idle_status, pc_ready, ctrl_status);

	return -EAGAIN;
}

int iris_vpu_power_off_controller(struct iris_core *core)
{
	/*
	 * DEBUG BUILD: the LPI/NOC handshakes that used to be here touch VPU
	 * registers.  When core init dies on the *first* register access they run
	 * in the error path and abort a second time, which is what took the
	 * machine down and hid the original fault.  Skip them so a failed probe
	 * is survivable and visible in dmesg.
	 */
	dev_err(core->dev, "IRIS-TRACE: power_off_controller: VPU MMIO skipped\n");
	iris_disable_unprepare_clock(core, IRIS_CTRL_CLK);
	iris_disable_unprepare_clock(core, IRIS_AXI1_CLK);
	iris_disable_unprepare_clock(core, IRIS_AXIC_CLK);
	iris_disable_unprepare_clock(core, IRIS_AXI_CLK);
	iris_disable_unprepare_clock(core, IRIS_AHB_CLK);
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN]);

	return 0;
}

void iris_vpu_power_off_hw(struct iris_core *core)
{
	dev_pm_genpd_set_hwmode(core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN], false);
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN]);
	iris_disable_unprepare_clock(core, IRIS_HW_CLK);
}

void iris_vpu_power_off(struct iris_core *core)
{
	dev_pm_opp_set_rate(core->dev, 0);
	core->iris_platform_data->vpu_ops->power_off_hw(core);
	core->iris_platform_data->vpu_ops->power_off_controller(core);
	iris_unset_icc_bw(core);

	if (!iris_vpu_watchdog(core, core->intr_status))
		disable_irq_nosync(core->irq);
}

int iris_vpu_power_on_controller(struct iris_core *core)
{
	u32 rst_tbl_size = core->iris_platform_data->clk_rst_tbl_size;
	int ret;

	ret = iris_enable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN]);
	if (ret)
		return ret;

	/* The vendor PIL enables "ahb" (VIDEO_CC_IRIS_AHB_CLK) as a proxy clock
	 * before the secure auth; without it TZ's register writes to the
	 * wrapper wedge the bus.  Enabling it through the clock framework also
	 * runs the shared RCG update handshake for its parent. */
	ret = iris_prepare_enable_clock(core, IRIS_AHB_CLK);
	if (ret && ret != -EINVAL)
		dev_err(core->dev, "IRIS-TRACE: ahb clock failed %d\n", ret);
	dev_err(core->dev, "IRIS-TRACE: ahb clock ret=%d\n", ret);

	ret = reset_control_bulk_reset(rst_tbl_size, core->resets);
	if (ret)
		goto err_disable_power;

	/* SC8180X has a separate AXI config-port clock (the port a CPU register
	 * access uses) and a second AXI clock.  Other platforms do not have
	 * them, so -EINVAL from a missing clock is not an error. */
	ret = iris_prepare_enable_clock(core, IRIS_AXIC_CLK);
	if (ret && ret != -EINVAL) {
		dev_err(core->dev, "IRIS-TRACE: axic clock failed %d\n", ret);
		goto err_disable_power;
	}
	dev_err(core->dev, "IRIS-TRACE: axic clock ret=%d\n", ret);

	ret = iris_prepare_enable_clock(core, IRIS_AXI_CLK);
	if (ret)
		goto err_disable_axic;

	ret = iris_prepare_enable_clock(core, IRIS_AXI1_CLK);
	if (ret && ret != -EINVAL)
		dev_err(core->dev, "IRIS-TRACE: axi1 clock failed %d\n", ret);

	ret = iris_prepare_enable_clock(core, IRIS_CTRL_CLK);
	if (ret)
		goto err_disable_axi1;

	return 0;

err_disable_axi1:
	iris_disable_unprepare_clock(core, IRIS_AXI1_CLK);
err_disable_axic:
	iris_disable_unprepare_clock(core, IRIS_AXIC_CLK);
	iris_disable_unprepare_clock(core, IRIS_AXI_CLK);
err_disable_power:
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN]);

	return ret;
}

int iris_vpu_power_on_hw(struct iris_core *core)
{
	int ret;

	ret = iris_enable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN]);
	if (ret)
		return ret;

	ret = iris_prepare_enable_clock(core, IRIS_HW_CLK);
	if (ret)
		goto err_disable_power;

	ret = dev_pm_genpd_set_hwmode(core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN], true);
	if (ret)
		goto err_disable_clock;

	return 0;

err_disable_clock:
	iris_disable_unprepare_clock(core, IRIS_HW_CLK);
err_disable_power:
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN]);

	return ret;
}

int iris_vpu_power_on(struct iris_core *core)
{
	struct clk *core_clk;
	u32 freq;
	int ret;

	ret = iris_set_icc_bw(core, INT_MAX);
	if (ret)
		goto err;

	dev_err(core->dev, "IRIS-TRACE: power_on: controller (pds/clocks/resets)\n");
	ret = core->iris_platform_data->vpu_ops->power_on_controller(core);
	if (ret)
		goto err_unvote_icc;

	dev_err(core->dev, "IRIS-TRACE: power_on: hw (vcodec0 pd/clk)\n");
	ret = core->iris_platform_data->vpu_ops->power_on_hw(core);
	if (ret)
		goto err_power_off_ctrl;

	/*
	 * Bottom of the table (IRIS_BOOT_FREQ), not ULONG_MAX: the vendor PIL
	 * brings the core up at 200 MHz and only scales up once an instance is
	 * streaming, and the secure auth plus the firmware handshake below run
	 * in that window.  ULONG_MAX made the OPP framework pick 533 MHz for it.
	 */
	freq = core->power.clk_freq ? core->power.clk_freq : IRIS_BOOT_FREQ;

	dev_err(core->dev, "IRIS-TRACE: power_on: opp set_rate(%u)\n", freq);
	dev_pm_opp_set_rate(core->dev, freq);

	/*
	 * IRIS_HW_CLK is the clock devm_pm_opp_set_clkname() binds the OPP table
	 * to, so its rate shows which entry was actually taken.  It and the AHB
	 * interface clock hang off the same video_cc_iris_clk_src, so this is
	 * also the rate the wrapper is talked to at.
	 */
	core_clk = iris_get_clk_by_type(core, IRIS_HW_CLK);
	if (core_clk)
		dev_err(core->dev, "IRIS-TRACE: power_on: opp done, IRIS_HW_CLK = %lu Hz\n",
			clk_get_rate(core_clk));
	else
		dev_err(core->dev, "IRIS-TRACE: power_on: opp done\n");

	dev_err(core->dev, "IRIS-TRACE: power_on: preset regs (first VPU write)\n");
	core->iris_platform_data->set_preset_registers(core);
	dev_err(core->dev, "IRIS-TRACE: power_on: preset regs done\n");

	dev_err(core->dev, "IRIS-TRACE: power_on: intr mask (VPU read)\n");
	iris_vpu_interrupt_init(core);
	dev_err(core->dev, "IRIS-TRACE: power_on: intr init done\n");
	core->intr_status = 0;
	enable_irq(core->irq);
	dev_err(core->dev, "IRIS-TRACE: power_on: irq enabled\n");

	return 0;

err_power_off_ctrl:
	core->iris_platform_data->vpu_ops->power_off_controller(core);
err_unvote_icc:
	iris_unset_icc_bw(core);
err:
	dev_err(core->dev, "power on failed\n");

	return ret;
}
