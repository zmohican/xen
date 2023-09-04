/*
 * Virtual Device MMIO Passthrough
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
 */

#ifndef _VPT_MMIO_H
#define _VPT_MMIO_H

#include <public/domctl.h>
#include <public/io/ring.h>
#include <public/io/console.h>
#include <xen/mm.h>

struct vpt_mmio_info {
	char name[32];
	paddr_t gpa_addr;
	paddr_t gpa_size;
	paddr_t mpa_addr;
	void * mpa_base;
	void * wenable;
	void * renable;
};

void add_vpt_mmio_region (struct domain *d, struct vpt_mmio_info *vpt);

#endif  /* _VPT_MMIO_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
