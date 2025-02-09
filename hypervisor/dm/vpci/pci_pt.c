/*-
* Copyright (c) 2011 NetApp, Inc.
* Copyright (c) 2018 Intel Corporation
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
* OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
* HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
* OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGE.
*
* $FreeBSD$
*/
#include <vm.h>
#include <errno.h>
#include <ept.h>
#include <mmu.h>
#include <logmsg.h>
#include "vpci_priv.h"

static inline uint32_t get_bar_base(uint32_t bar)
{
	return bar & PCIM_BAR_MEM_BASE;
}

/**
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->vpci->vm != NULL
 */
int32_t vdev_pt_read_cfg(const struct pci_vdev *vdev, uint32_t offset, uint32_t bytes, uint32_t *val)
{
	int32_t ret = -ENODEV;

	if (is_prelaunched_vm(vdev->vpci->vm) && is_bar_offset(vdev->nr_bars, offset)) {
		*val = pci_vdev_read_cfg(vdev, offset, bytes);
		ret = 0;
	}

	return ret;
}

/**
* @pre vdev != NULL
* @pre vdev->vpci != NULL
* @pre vdev->vpci->vm != NULL
* @pre vdev->pdev != NULL
* @pre vdev->pdev->msix.table_bar < vdev->nr_bars
*/
void vdev_pt_remap_msix_table_bar(struct pci_vdev *vdev)
{
	uint32_t i;
	uint64_t addr_hi, addr_lo;
	struct pci_msix *msix = &vdev->msix;
	struct pci_pdev *pdev = vdev->pdev;
	struct pci_bar *bar;

	ASSERT(vdev->pdev->msix.table_bar < vdev->nr_bars, "msix->table_bar is out of range");

	/* Mask all table entries */
	for (i = 0U; i < msix->table_count; i++) {
		msix->table_entries[i].vector_control = PCIM_MSIX_VCTRL_MASK;
		msix->table_entries[i].addr = 0U;
		msix->table_entries[i].data = 0U;
	}

	bar = &pdev->bar[msix->table_bar];
	if (bar != NULL) {
		msix->mmio_hpa = bar->base;
		if (is_prelaunched_vm(vdev->vpci->vm)) {
			msix->mmio_gpa = vdev->bar[msix->table_bar].base;
		} else {
			msix->mmio_gpa = sos_vm_hpa2gpa(bar->base);
		}
		msix->mmio_size = bar->size;
	}

	/*
	 *    For SOS:
	 *    --------
	 *    MSI-X Table BAR Contains:
	 *    Other Info + Tables + PBA	        Other info already mapped into EPT (since SOS)
	 *    					Tables are handled by HV MMIO handler (4k adjusted up and down)
	 *    						and remaps interrupts
	 *    					PBA already mapped into EPT (since SOS)
	 *
	 *    Other Info + Tables		Other info already mapped into EPT (since SOS)
	 *					Tables are handled by HV MMIO handler (4k adjusted up and down)
	 *						and remaps interrupts
	 *
	 *    Tables				Tables are handled by HV MMIO handler (4k adjusted up and down)
	 *    						and remaps interrupts
	 *
	 *    For UOS (launched by DM):
	 *    -------------------------
	 *    MSI-X Table BAR Contains:
	 *    Other Info + Tables + PBA		Other info  mapped into EPT (4k adjusted) by DM
	 *    					Tables are handled by DM MMIO handler (4k adjusted up and down) and SOS writes to tables,
	 *    						intercepted by HV MMIO handler and HV remaps interrupts
	 *    					PBA already mapped into EPT by DM
	 *
	 *    Other Info + Tables		Other info mapped into EPT by DM
	 *    					Tables are handled by DM MMIO handler (4k adjusted up and down) and SOS writes to tables,
	 *    						intercepted by HV MMIO handler and HV remaps interrupts.
	 *
	 *    Tables				Tables are handled by DM MMIO handler (4k adjusted up and down) and SOS writes to tables,
	 *    						intercepted by HV MMIO handler and HV remaps interrupts.
	 *
	 *    For Pre-launched VMs (no SOS/DM):
	 *    --------------------------------
	 *    MSI-X Table BAR Contains:
	 *    All 3 cases:			Writes to MMIO region in MSI-X Table BAR handled by HV MMIO handler
	 *    					If the offset falls within the MSI-X table [offset, offset+tables_size), HV remaps
	 *    						interrupts.
	 *    					Else, HV writes/reads to/from the corresponding HPA
	 */


	if (msix->mmio_gpa != 0UL) {
		if (is_prelaunched_vm(vdev->vpci->vm)) {
			addr_hi = msix->mmio_gpa + msix->mmio_size;
			addr_lo = msix->mmio_gpa;
		} else {
			/*
			* PCI Spec: a BAR may also map other usable address space that is not associated
			* with MSI-X structures, but it must not share any naturally aligned 4 KB
			* address range with one where either MSI-X structure resides.
			* The MSI-X Table and MSI-X PBA are permitted to co-reside within a naturally
			* aligned 4 KB address range.
			*
			* If PBA or others reside in the same BAR with MSI-X Table, devicemodel could
			* emulate them and maps these memory range at the 4KB boundary. Here, we should
			* make sure only intercept the minimum number of 4K pages needed for MSI-X table.
			*/

			/* The higher boundary of the 4KB aligned address range for MSI-X table */
			addr_hi = msix->mmio_gpa + msix->table_offset + (msix->table_count * MSIX_TABLE_ENTRY_SIZE);
			addr_hi = round_page_up(addr_hi);

			/* The lower boundary of the 4KB aligned address range for MSI-X table */
			addr_lo = round_page_down(msix->mmio_gpa + msix->table_offset);
		}

		register_mmio_emulation_handler(vdev->vpci->vm, vmsix_table_mmio_access_handler,
			addr_lo, addr_hi, vdev);
	}
}

/**
 * @brief Remaps guest MMIO BARs other than MSI-x Table BAR
 * This API is invoked upon guest re-programming PCI BAR with MMIO region
 * after a new vbar is set.
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->vpci->vm != NULL
 */
static void vdev_pt_remap_generic_mem_vbar(const struct pci_vdev *vdev, uint32_t idx, uint32_t new_base)
{
	struct acrn_vm *vm = vdev->vpci->vm;

	if (vdev->bar[idx].base != 0UL) {
		ept_del_mr(vm, (uint64_t *)vm->arch_vm.nworld_eptp,
			vdev->bar[idx].base,
			vdev->bar[idx].size);
	}

	if (new_base != 0U) {
		/* Map the physical BAR in the guest MMIO space */
		ept_add_mr(vm, (uint64_t *)vm->arch_vm.nworld_eptp,
			vdev->pdev->bar[idx].base, /* HPA */
			new_base, /*GPA*/
			vdev->bar[idx].size,
			EPT_WR | EPT_RD | EPT_UNCACHED);
	}
}

/**
 * @pre vdev != NULL
 * @pre (vdev->bar[idx].type == PCIBAR_NONE) || (vdev->bar[idx].type == PCIBAR_MEM32)
 */
static void vdev_pt_write_vbar(struct pci_vdev *vdev, uint32_t offset, uint32_t val)
{
	uint32_t idx;
	uint32_t new_bar, mask;
	bool bar_update_normal;
	bool is_msix_table_bar;

	new_bar = 0U;
	idx = (offset - pci_bar_offset(0U)) >> 2U;
	mask = ~(vdev->bar[idx].size - 1U);

	switch (vdev->bar[idx].type) {
	case PCIBAR_NONE:
		vdev->bar[idx].base = 0UL;
		break;

	case PCIBAR_MEM32:
		bar_update_normal = (val != (uint32_t)~0U);
		is_msix_table_bar = (has_msix_cap(vdev) && (idx == vdev->msix.table_bar));
		new_bar = val & mask;
		if (bar_update_normal) {
			if (is_msix_table_bar) {
				vdev->bar[idx].base = get_bar_base(new_bar);
				vdev_pt_remap_msix_table_bar(vdev);
			} else {
				vdev_pt_remap_generic_mem_vbar(vdev, idx,
					get_bar_base(new_bar));

				vdev->bar[idx].base = get_bar_base(new_bar);
			}
		}
		break;

	default:
		/* Should never reach here, init_vdev_pt() only sets vbar type to PCIBAR_NONE and PCIBAR_MEM32 */
		break;
	}

	pci_vdev_write_cfg_u32(vdev, offset, new_bar);
}

/**
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->vpci->vm != NULL
 * bar write access must be 4 bytes and offset must also be 4 bytes aligned, it will be dropped otherwise
 */
int32_t vdev_pt_write_cfg(struct pci_vdev *vdev, uint32_t offset, uint32_t bytes, uint32_t val)
{
	int32_t ret = -ENODEV;

	/* bar write access must be 4 bytes and offset must also be 4 bytes aligned */
	if (is_prelaunched_vm(vdev->vpci->vm) && is_bar_offset(vdev->nr_bars, offset)
		&& (bytes == 4U) && ((offset & 0x3U) == 0U)) {
		vdev_pt_write_vbar(vdev, offset, val);
		ret = 0;
	}

	return ret;
}

/**
 * For bar emulation, currently only MMIO is supported and bar size cannot be greater than 4GB
 * @pre bar != NULL
 */
static inline bool is_bar_supported(const struct pci_bar *bar)
{
	return (is_mmio_bar(bar) && is_valid_bar_size(bar));
}

/**
 * PCI base address register (bar) virtualization:
 *
 * Virtualize the PCI bars (up to 6 bars at byte offset 0x10~0x24 for type 0 PCI device,
 * 2 bars at byte offset 0x10-0x14 for type 1 PCI device) of the PCI configuration space
 * header.
 *
 * pbar: bar for the physical PCI device (pci_pdev), the value of pbar (hpa) is assigned
 * by platform firmware during boot. It is assumed a valid hpa is always assigned to a
 * mmio pbar, hypervisor shall not change the value of a pbar.
 *
 * vbar: for each pci_pdev, it has a virtual PCI device (pci_vdev) counterpart. pci_vdev
 * virtualizes all the bars (called vbars). a vbar can be initialized by hypervisor by
 * assigning a gpa to it; if vbar has a value of 0 (unassigned), guest may assign
 * and program a gpa to it. The guest only sees the vbars, it will not see and can
 * never change the pbars.
 *
 * Hypervisor traps guest changes to the mmio vbar (gpa) to establish ept mapping
 * between vbar(gpa) and pbar(hpa). pbar should always align on 4K boundary.
 *
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->vpci->vm != NULL
 * @pre vdev->pdev != NULL
 */
void init_vdev_pt(struct pci_vdev *vdev)
{
	uint32_t idx;
	struct pci_bar *pbar, *vbar;
	uint16_t pci_command;

	vdev->nr_bars = vdev->pdev->nr_bars;

	ASSERT(vdev->nr_bars > 0U, "vdev->nr_bars should be greater than 0!");

	if (is_prelaunched_vm(vdev->vpci->vm)) {
		for (idx = 0U; idx < vdev->nr_bars; idx++) {
			pbar = &vdev->pdev->bar[idx];
			vbar = &vdev->bar[idx];

			vbar->base = 0UL;
			if (is_bar_supported(pbar)) {
				/**
				 * If vbar->base is 0 (unassigned), Linux kernel will reprogram the vbar on
				 * its bar size boundary, so in order to ensure the vbar allocated by guest
				 * is 4k aligned, set its size to be 4K aligned.
				 */
				vbar->size = round_page_up(pbar->size);

				/**
				 * Only 32-bit bar is supported for now so both PCIBAR_MEM32 and PCIBAR_MEM64
				 * are reported to guest as PCIBAR_MEM32
				 */
				vbar->type = PCIBAR_MEM32;

				/* Set the new vbar base */
				if (vdev->ptdev_config->vbar[idx] != 0UL) {
					vdev_pt_write_vbar(vdev, pci_bar_offset(idx), (uint32_t)(vdev->ptdev_config->vbar[idx]));
				}
			} else {
				vbar->size = 0UL;
				vbar->type = PCIBAR_NONE;
			}
		}

		pci_command = (uint16_t)pci_pdev_read_cfg(vdev->pdev->bdf, PCIR_COMMAND, 2U);
		/* Disable INTX */
		pci_command |= 0x400U;
		pci_pdev_write_cfg(vdev->pdev->bdf, PCIR_COMMAND, 2U, pci_command);
	}
}
