/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/arm64/arm_mmu.h>
#include <zephyr/sys/util.h>

static const struct arm_mmu_region mmu_regions[] = {
	/* Firmware secondary-core spin table at 0xd8..0xf0. */
	MMU_REGION_FLAT_ENTRY("spin-table", 0x00000000, 0x1000,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_SECURE),

	/* BCM2835 legacy ARM interrupt controller. */
	MMU_REGION_FLAT_ENTRY("armctrl", 0x3f00b000, 0x1000,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_SECURE),

	/* BCM2836 local timer and mailbox interrupt controller. */
	MMU_REGION_FLAT_ENTRY("local-intc", 0x40000000, 0x1000,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_SECURE),
};

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};
