/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/arm64/cpu.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/irq.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define LOCAL_BASE			0x40000000UL
#define LOCAL_GPU_IRQ_ROUTING		(LOCAL_BASE + 0x0cU)
#define LOCAL_TIMER_IRQ_CONTROL(core)	(LOCAL_BASE + 0x40U + ((core) * 4U))
#define LOCAL_MBOX_IRQ_CONTROL(core)	(LOCAL_BASE + 0x50U + ((core) * 4U))
#define LOCAL_IRQ_SOURCE(core)		(LOCAL_BASE + 0x60U + ((core) * 4U))
#define LOCAL_MBOX_SET(core, mbox)	(LOCAL_BASE + 0x80U + ((core) * 0x10U) + \
					 ((mbox) * 4U))
#define LOCAL_MBOX_CLR(core, mbox)	(LOCAL_BASE + 0xc0U + ((core) * 0x10U) + \
					 ((mbox) * 4U))

#define ARMCTRL_BASE			0x3f00b200UL
#define ARMCTRL_BASIC_PENDING		(ARMCTRL_BASE + 0x00U)
#define ARMCTRL_PENDING_1		(ARMCTRL_BASE + 0x04U)
#define ARMCTRL_PENDING_2		(ARMCTRL_BASE + 0x08U)
#define ARMCTRL_ENABLE_1		(ARMCTRL_BASE + 0x10U)
#define ARMCTRL_ENABLE_2		(ARMCTRL_BASE + 0x14U)
#define ARMCTRL_DISABLE_1		(ARMCTRL_BASE + 0x1cU)
#define ARMCTRL_DISABLE_2		(ARMCTRL_BASE + 0x20U)

#define LOCAL_TIMER_IRQ_BASE		0U
#define LOCAL_MBOX_IRQ_BASE		4U
#define ARMCTRL_IRQ_BASE		32U
#define ARMCTRL_IRQ_COUNT		64U
#define LOCAL_GPU_IRQ			BIT(8)

static uint32_t timer_enabled[CONFIG_MP_MAX_NUM_CPUS];
static uint32_t mailbox_enabled[CONFIG_MP_MAX_NUM_CPUS];
static uint32_t armctrl_enabled[2];

static unsigned int current_core(void)
{
	return MPIDR_AFFLVL(GET_MPIDR(), 0);
}

void z_soc_irq_init(void)
{
	unsigned int core = current_core();
	unsigned int mailbox;

	sys_write32(0U, LOCAL_TIMER_IRQ_CONTROL(core));
	sys_write32(0U, LOCAL_MBOX_IRQ_CONTROL(core));
	for (mailbox = 0U; mailbox < 4U; mailbox++) {
		sys_write32(UINT32_MAX, LOCAL_MBOX_CLR(core, mailbox));
	}
	timer_enabled[core] = 0U;
	mailbox_enabled[core] = 0U;

	if (core == 0U) {
		/* Route all legacy GPU peripheral interrupts to the boot core. */
		sys_write32(0U, LOCAL_GPU_IRQ_ROUTING);
		sys_write32(UINT32_MAX, ARMCTRL_DISABLE_1);
		sys_write32(UINT32_MAX, ARMCTRL_DISABLE_2);
		armctrl_enabled[0] = 0U;
		armctrl_enabled[1] = 0U;
	}
	barrier_dsync_fence_full();
}

void z_soc_irq_enable(unsigned int irq)
{
	unsigned int core = current_core();

	if (irq < LOCAL_MBOX_IRQ_BASE) {
		timer_enabled[core] |= BIT(irq);
		sys_write32(timer_enabled[core], LOCAL_TIMER_IRQ_CONTROL(core));
		return;
	}

	if (irq < ARMCTRL_IRQ_BASE) {
		unsigned int mailbox = irq - LOCAL_MBOX_IRQ_BASE;

		if (mailbox < 4U) {
			mailbox_enabled[core] |= BIT(mailbox);
			sys_write32(mailbox_enabled[core], LOCAL_MBOX_IRQ_CONTROL(core));
		}
		return;
	}

	irq -= ARMCTRL_IRQ_BASE;
	if (irq < 32U) {
		armctrl_enabled[0] |= BIT(irq);
		sys_write32(BIT(irq), ARMCTRL_ENABLE_1);
	} else if (irq < ARMCTRL_IRQ_COUNT) {
		irq -= 32U;
		armctrl_enabled[1] |= BIT(irq);
		sys_write32(BIT(irq), ARMCTRL_ENABLE_2);
	}
}

void z_soc_irq_disable(unsigned int irq)
{
	unsigned int core = current_core();

	if (irq < LOCAL_MBOX_IRQ_BASE) {
		timer_enabled[core] &= ~BIT(irq);
		sys_write32(timer_enabled[core], LOCAL_TIMER_IRQ_CONTROL(core));
		return;
	}

	if (irq < ARMCTRL_IRQ_BASE) {
		unsigned int mailbox = irq - LOCAL_MBOX_IRQ_BASE;

		if (mailbox < 4U) {
			mailbox_enabled[core] &= ~BIT(mailbox);
			sys_write32(mailbox_enabled[core], LOCAL_MBOX_IRQ_CONTROL(core));
		}
		return;
	}

	irq -= ARMCTRL_IRQ_BASE;
	if (irq < 32U) {
		armctrl_enabled[0] &= ~BIT(irq);
		sys_write32(BIT(irq), ARMCTRL_DISABLE_1);
	} else if (irq < ARMCTRL_IRQ_COUNT) {
		irq -= 32U;
		armctrl_enabled[1] &= ~BIT(irq);
		sys_write32(BIT(irq), ARMCTRL_DISABLE_2);
	}
}

int z_soc_irq_is_enabled(unsigned int irq)
{
	unsigned int core = current_core();

	if (irq < LOCAL_MBOX_IRQ_BASE) {
		return (timer_enabled[core] & BIT(irq)) != 0U;
	}

	if (irq < ARMCTRL_IRQ_BASE) {
		unsigned int mailbox = irq - LOCAL_MBOX_IRQ_BASE;

		return mailbox < 4U && (mailbox_enabled[core] & BIT(mailbox)) != 0U;
	}

	irq -= ARMCTRL_IRQ_BASE;
	if (irq < 32U) {
		return (armctrl_enabled[0] & BIT(irq)) != 0U;
	}
	if (irq < ARMCTRL_IRQ_COUNT) {
		return (armctrl_enabled[1] & BIT(irq - 32U)) != 0U;
	}

	return 0;
}

void z_soc_irq_priority_set(unsigned int irq, unsigned int prio, uint32_t flags)
{
	ARG_UNUSED(irq);
	ARG_UNUSED(prio);
	ARG_UNUSED(flags);
}

unsigned int z_soc_irq_get_active(void)
{
	unsigned int core = current_core();
	uint32_t source = sys_read32(LOCAL_IRQ_SOURCE(core));
	uint32_t pending;

	pending = source & timer_enabled[core] & GENMASK(3, 0);
	if (pending != 0U) {
		unsigned int irq = LOCAL_TIMER_IRQ_BASE + find_lsb_set(pending) - 1U;

		sys_write32(timer_enabled[core] & ~BIT(irq),
			    LOCAL_TIMER_IRQ_CONTROL(core));
		return irq;
	}

	pending = ((source >> 4) & mailbox_enabled[core]) & GENMASK(3, 0);
	if (pending != 0U) {
		unsigned int mailbox = find_lsb_set(pending) - 1U;

		sys_write32(mailbox_enabled[core] & ~BIT(mailbox),
			    LOCAL_MBOX_IRQ_CONTROL(core));
		sys_write32(UINT32_MAX, LOCAL_MBOX_CLR(core, mailbox));
		barrier_dsync_fence_full();
		return LOCAL_MBOX_IRQ_BASE + mailbox;
	}

	if ((source & LOCAL_GPU_IRQ) != 0U) {
		pending = sys_read32(ARMCTRL_PENDING_1) & armctrl_enabled[0];
		if (pending != 0U) {
			unsigned int irq = find_lsb_set(pending) - 1U;

			sys_write32(BIT(irq), ARMCTRL_DISABLE_1);
			return ARMCTRL_IRQ_BASE + irq;
		}

		pending = sys_read32(ARMCTRL_PENDING_2) & armctrl_enabled[1];
		if (pending != 0U) {
			unsigned int irq = find_lsb_set(pending) - 1U;

			sys_write32(BIT(irq), ARMCTRL_DISABLE_2);
			return ARMCTRL_IRQ_BASE + 32U + irq;
		}
	}

	return CONFIG_NUM_IRQS;
}

void z_soc_irq_eoi(unsigned int irq)
{
	unsigned int core = current_core();

	if (irq < LOCAL_MBOX_IRQ_BASE) {
		sys_write32(timer_enabled[core], LOCAL_TIMER_IRQ_CONTROL(core));
		return;
	}

	if (irq >= LOCAL_MBOX_IRQ_BASE && irq < (LOCAL_MBOX_IRQ_BASE + 4U)) {
		sys_write32(mailbox_enabled[core], LOCAL_MBOX_IRQ_CONTROL(core));
		return;
	}

	if (irq >= ARMCTRL_IRQ_BASE) {
		unsigned int arm_irq = irq - ARMCTRL_IRQ_BASE;

		if (arm_irq < 32U) {
			if ((armctrl_enabled[0] & BIT(arm_irq)) != 0U) {
				sys_write32(BIT(arm_irq), ARMCTRL_ENABLE_1);
			}
		} else if (arm_irq < ARMCTRL_IRQ_COUNT) {
			arm_irq -= 32U;
			if ((armctrl_enabled[1] & BIT(arm_irq)) != 0U) {
				sys_write32(BIT(arm_irq), ARMCTRL_ENABLE_2);
			}
		}
	}
}

void z_soc_irq_secondary_init(void)
{
	z_soc_irq_init();
}

void z_soc_irq_send_ipi(unsigned int ipi, uint64_t target_mpidr)
{
	unsigned int core = MPIDR_AFFLVL(target_mpidr, 0);
	unsigned int mailbox = ipi - LOCAL_MBOX_IRQ_BASE;

	if (core < CONFIG_MP_MAX_NUM_CPUS && mailbox < 4U) {
		sys_write32(1U, LOCAL_MBOX_SET(core, mailbox));
		barrier_dsync_fence_full();
		sev();
	}
}

#ifdef CONFIG_FPU_SHARING
bool z_soc_irq_is_pending(unsigned int irq)
{
	unsigned int mailbox = irq - LOCAL_MBOX_IRQ_BASE;

	if (mailbox >= 4U) {
		return false;
	}

	return (sys_read32(LOCAL_IRQ_SOURCE(current_core())) &
		BIT(mailbox + LOCAL_MBOX_IRQ_BASE)) != 0U;
}

void z_soc_irq_clear_pending(unsigned int irq)
{
	unsigned int mailbox = irq - LOCAL_MBOX_IRQ_BASE;

	if (mailbox < 4U) {
		sys_write32(UINT32_MAX, LOCAL_MBOX_CLR(current_core(), mailbox));
		barrier_dsync_fence_full();
	}
}
#endif
