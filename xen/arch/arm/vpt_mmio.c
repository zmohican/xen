/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/arm/vpt_mmio.c
 *
 * Device MMIO Virtual Passthrough 
 */

#include <xen/errno.h>
#include <xen/event.h>
#include <xen/guest_access.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/console.h>
#include <xen/serial.h>
#include <public/domctl.h>
#include <public/io/console.h>
#include <asm/pl011-uart.h>
#include <asm/vgic-emul.h>
#include <asm/vpt_mmio.h>
#include <asm/vreg.h>


static int vpt_mmio_read(struct vcpu *v,
                            mmio_info_t *info,
                            register_t *r,
                            void *priv)
{
	struct vpt_mmio_info *vpt = (struct vpt_mmio_info *)priv;
	u32 offset = (uint32_t)(info->gpa - vpt->gpa_addr);
	u64 renable = 0;
	u64 data = 0;

	// intentionally no alignment check
	switch (info->dabt.size)
	{
	    case DABT_BYTE:		data = *((volatile u8 *)(vpt->mpa_base + offset));  renable = (vpt->renable) ? *((u8 *)(vpt->renable + offset)) : 0xff ; break;
	    case DABT_HALF_WORD:	data = *((volatile u16*)(vpt->mpa_base + offset));  renable = (vpt->renable) ? *((u8 *)(vpt->renable + offset)) : 0xffff; break;
	    case DABT_WORD:		data = *((volatile u32*)(vpt->mpa_base + offset));  renable = (vpt->renable) ? *((u8 *)(vpt->renable + offset)) : 0xffffffff; break;
	    case DABT_DOUBLE_WORD:	data = *((volatile u64*)(vpt->mpa_base + offset));  renable = (vpt->renable) ? *((u8 *)(vpt->renable + offset)) : 0xffffffffffffffff; break;
	}
	*r = (data & renable);

	//printk("%s : %s data:0x%x offset:0x%x\n", __func__, vpt->name, data, offset);

	return 1;
}
static int vpt_mmio_write(struct vcpu *v,
                             mmio_info_t *info,
                             register_t r,
                             void *priv)
{
	struct vpt_mmio_info *vpt = (struct vpt_mmio_info *)priv;
	u32 offset = (uint32_t)(info->gpa - vpt->gpa_addr);
	u64 old_data, data;
	u64 wenable = vpt->wenable ? *(u64 *)(vpt->wenable + offset) : 0xffffffffffffffff;

	switch (info->dabt.size)
	{
	    case DABT_BYTE:		old_data = *((volatile u8 *)(vpt->mpa_base + offset));  wenable = (vpt->wenable) ? *((u8 *)(vpt->wenable + offset)) : 0xff ; break;
	    case DABT_HALF_WORD:	old_data = *((volatile u16*)(vpt->mpa_base + offset));  wenable = (vpt->wenable) ? *((u8 *)(vpt->wenable + offset)) : 0xffff; break;
	    case DABT_WORD:		old_data = *((volatile u32*)(vpt->mpa_base + offset));  wenable = (vpt->wenable) ? *((u8 *)(vpt->wenable + offset)) : 0xffffffff; break;
	    case DABT_DOUBLE_WORD:	old_data = *((volatile u64*)(vpt->mpa_base + offset));  wenable = (vpt->wenable) ? *((u8 *)(vpt->wenable + offset)) : 0xffffffffffffffff; break;
	}
	
	data = r;

	//printk("%s : %s data:0x%x offset:0x%x\n", __func__, vpt->name, data, offset);

	data = (old_data & ~wenable) | (data & wenable);

	switch (info->dabt.size)
	{
	    case DABT_BYTE:		*((u8 *)(vpt->mpa_base + offset)) = data;  break;
	    case DABT_HALF_WORD:	*((u16*)(vpt->mpa_base + offset)) = data;  break;
	    case DABT_WORD:		*((u32*)(vpt->mpa_base + offset)) = data;  break;
	    case DABT_DOUBLE_WORD:	*((u64*)(vpt->mpa_base + offset)) = data;  break;
	}

	return 1;
}

static const struct mmio_handler_ops vpt_mmio_handler = {
    .read  = vpt_mmio_read,
    .write = vpt_mmio_write,
};

void add_vpt_mmio_region (struct domain *d, struct vpt_mmio_info *vpt)
{
	register_mmio_handler(d, &vpt_mmio_handler, vpt->gpa_addr, vpt->gpa_size, vpt);
}
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
