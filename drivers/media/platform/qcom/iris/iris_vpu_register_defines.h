/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_VPU_REGISTER_DEFINES_H__
#define __IRIS_VPU_REGISTER_DEFINES_H__

/*
 * TEST: SC8180X (SM8150 generation) uses the older Venus 4xx register map,
 * matching the vendor's downstream IRIS1 header (hfi_io_common.h) and the
 * mainline venus driver's non-V6 layout.  The values the iris driver had were
 * the Venus 6xx ones (SM8250 and later), so every access went to an unmapped
 * address - which is what produced the external aborts.
 */
#define VCODEC_BASE_OFFS			0x00000000
#define CPU_BASE_OFFS				0x000C0000
#define WRAPPER_BASE_OFFS			0x000E0000

#define CPU_CS_BASE_OFFS			(CPU_BASE_OFFS + 0x12000)

#define WRAPPER_CORE_POWER_STATUS		(WRAPPER_BASE_OFFS + 0x80)

/* Wrapper hardware revision, decoded the way the downstream driver does
 * (vidc_hfi_io.h): major 31:28, minor 23:16, step 15:0.  Reading it answers
 * whether this core really is the "Iris v2.xx" the sm8250 binding describes,
 * given that its firmware string is VIDEO.IR.1.2. */
#define WRAPPER_HW_VERSION			(WRAPPER_BASE_OFFS + 0x00)
#define WRAPPER_HW_VERSION_MAJOR_MASK		0x78000000
#define WRAPPER_HW_VERSION_MAJOR_SHIFT		28
#define WRAPPER_HW_VERSION_MINOR_MASK		0x0fff0000
#define WRAPPER_HW_VERSION_MINOR_SHIFT		16
#define WRAPPER_HW_VERSION_STEP_MASK		0x0000ffff

/* VCODEC core0 NoC error block (vendor vidc_hfi_io.h: vidc base + 0x4000, with
 * the CVP core's copy at + 0xC000).  It lives inside this node's window, and a
 * NoC error raised by the VPU is what a "reset from below Linux" looks like:
 * ERRVLD is the sticky logged-error bit, ERRCLR acknowledges it, ERRLOG0 is
 * the first log word. */
#define VCODEC_CORE0_VIDEO_NOC_BASE_OFFS	0x00004000
#define VCODEC_CVP_NOC_BASE_OFFS		0x0000c000
#define VCODEC_NOC_ERR_ERRVLD_LOW_OFFS		0x0510
#define VCODEC_NOC_ERR_ERRCLR_LOW_OFFS		0x0518
#define VCODEC_NOC_ERR_ERRLOG0_LOW_OFFS		0x0520
#define VCODEC_NOC_ERR_ERRLOG0_HIGH_OFFS	0x0524

/* Firmware/CP address window the core is booted from.  hfi_venus.c's
 * venus_reset_cpu() writes these (same offsets in both maps) before the
 * secure auth releases the core; with them zeroed the core fetches from
 * address 0 and the bus wedges. */
#define WRAPPER_CPA_START_ADDR			(WRAPPER_BASE_OFFS + 0x1020)
#define WRAPPER_CPA_END_ADDR			(WRAPPER_BASE_OFFS + 0x1024)
#define WRAPPER_FW_START_ADDR			(WRAPPER_BASE_OFFS + 0x1028)
#define WRAPPER_FW_END_ADDR			(WRAPPER_BASE_OFFS + 0x102C)
#define WRAPPER_NONPIX_START_ADDR		(WRAPPER_BASE_OFFS + 0x1030)
#define WRAPPER_NONPIX_END_ADDR			(WRAPPER_BASE_OFFS + 0x1034)
#define WRAPPER_CPU_CLOCK_CONFIG		(WRAPPER_BASE_OFFS + 0x2000)
#define WRAPPER_CPU_CGC_DIS			(WRAPPER_BASE_OFFS + 0x2010)
#define WRAPPER_A9SS_SW_RESET			(WRAPPER_BASE_OFFS + 0x3000)

#endif
