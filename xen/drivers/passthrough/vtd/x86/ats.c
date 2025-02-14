/*
 * Copyright (c) 2006, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Allen Kay <allen.m.kay@intel.com>
 */

#include <xen/sched.h>
#include <xen/iommu.h>
#include <xen/time.h>
#include <xen/pci.h>
#include <xen/pci_regs.h>
#include <asm/msi.h>
#include "../iommu.h"
#include "../dmar.h"
#include "../vtd.h"
#include "../extern.h"
#include "../../ats.h"

static LIST_HEAD(ats_dev_drhd_units);

struct acpi_drhd_unit *find_ats_dev_drhd(struct vtd_iommu *iommu)
{
    struct acpi_drhd_unit *drhd;
    list_for_each_entry ( drhd, &ats_dev_drhd_units, list )
    {
        if ( drhd->iommu == iommu )
            return drhd;
    }
    return NULL;
}

int ats_device(const struct pci_dev *pdev, const struct acpi_drhd_unit *drhd)
{
    struct acpi_drhd_unit *ats_drhd;
    int pos;

    if ( !ats_enabled || !iommu_qinval )
        return 0;

    if ( !ecap_queued_inval(drhd->iommu->ecap) ||
         !ecap_dev_iotlb(drhd->iommu->ecap) )
        return 0;

    if ( !acpi_find_matched_atsr_unit(pdev) )
        return 0;

    ats_drhd = find_ats_dev_drhd(drhd->iommu);
    pos = pci_find_ext_capability(pdev->seg, pdev->bus, pdev->devfn,
                                  PCI_EXT_CAP_ID_ATS);

    if ( pos && (ats_drhd == NULL) )
    {
        ats_drhd = xmalloc(struct acpi_drhd_unit);
        if ( !ats_drhd )
            return -ENOMEM;
        *ats_drhd = *drhd;
        list_add_tail(&ats_drhd->list, &ats_dev_drhd_units);
    }
    return pos;
}

static bool device_in_domain(const struct vtd_iommu *iommu,
                             const struct pci_dev *pdev, uint16_t did)
{
    struct root_entry *root_entry;
    struct context_entry *ctxt_entry = NULL;
    unsigned int tt;
    bool found = false;

    if ( unlikely(!iommu->root_maddr) )
    {
        ASSERT_UNREACHABLE();
        return false;
    }

    root_entry = map_vtd_domain_page(iommu->root_maddr);
    if ( !root_present(root_entry[pdev->bus]) )
        goto out;

    ctxt_entry = map_vtd_domain_page(root_entry[pdev->bus].val);
    if ( context_domain_id(ctxt_entry[pdev->devfn]) != did )
        goto out;

    tt = context_translation_type(ctxt_entry[pdev->devfn]);
    if ( tt != CONTEXT_TT_DEV_IOTLB )
        goto out;

    found = true;
out:
    if ( root_entry )
        unmap_vtd_domain_page(root_entry);

    if ( ctxt_entry )
        unmap_vtd_domain_page(ctxt_entry);

    return found;
}

int dev_invalidate_iotlb(struct vtd_iommu *iommu, u16 did,
    u64 addr, unsigned int size_order, u64 type)
{
    struct pci_dev *pdev, *temp;
    int ret = 0;

    if ( !ecap_dev_iotlb(iommu->ecap) )
        return ret;

    list_for_each_entry_safe( pdev, temp, &iommu->ats_devices, ats.list )
    {
        bool sbit;
        int rc = 0;

        switch ( type )
        {
        case DMA_TLB_DSI_FLUSH:
            if ( !device_in_domain(iommu, pdev, did) )
                break;
            /* fall through if DSI condition met */
        case DMA_TLB_GLOBAL_FLUSH:
            /* invalidate all translations: sbit=1,bit_63=0,bit[62:12]=1 */
            sbit = 1;
            addr = (~0UL << PAGE_SHIFT_4K) & 0x7FFFFFFFFFFFFFFF;
            rc = qinval_device_iotlb_sync(iommu, pdev, did, sbit, addr);
            break;
        case DMA_TLB_PSI_FLUSH:
            if ( !device_in_domain(iommu, pdev, did) )
                break;

            /* if size <= 4K, set sbit = 0, else set sbit = 1 */
            sbit = size_order ? 1 : 0;

            /* clear lower bits */
            addr &= ~0UL << PAGE_SHIFT_4K;

            /* if sbit == 1, zero out size_order bit and set lower bits to 1 */
            if ( sbit )
            {
                addr &= ~((u64)PAGE_SIZE_4K << (size_order - 1));
                addr |= (((u64)1 << (size_order - 1)) - 1) << PAGE_SHIFT_4K;
            }

            rc = qinval_device_iotlb_sync(iommu, pdev, did, sbit, addr);
            break;
        default:
            dprintk(XENLOG_WARNING VTDPREFIX, "invalid vt-d flush type\n");
            return -EOPNOTSUPP;
        }

        if ( !ret )
            ret = rc;
    }

    return ret;
}
