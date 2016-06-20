/**
 * Copyright (c) 2015-2016 Quantenna Communications, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 **/

#ifndef _QTN_FMAC_PCIE_H_
#define _QTN_FMAC_PCIE_H_

#include <linux/dma-mapping.h>
#include <linux/io.h>

#include "pcie_regs_pearl.h"
#include "pcie_ipc.h"
#include "shm_ipc.h"

/* */

struct qtnf_pcie_bus_priv {
	struct pci_dev  *pdev;

	/* lock for irq configuration changes */
	spinlock_t irq_lock;

	/* lock for tx operations */
	spinlock_t tx_lock;
	u8 msi_enabled;
	int mps;

	struct workqueue_struct *workqueue;
	struct tasklet_struct reclaim_tq;

	void __iomem *sysctl_bar;
	void __iomem *epmem_bar;
	void __iomem *dmareg_bar;

	struct qtnf_shm_ipc shm_ipc_ep_in;
	struct qtnf_shm_ipc shm_ipc_ep_out;

	struct qtnf_pcie_bda __iomem *bda;
	void __iomem *pcie_reg_base;

	u16 tx_bd_num;
	u16 rx_bd_num;

	struct sk_buff **tx_skb;
	struct sk_buff **rx_skb;

	struct qtnf_tx_bd *tx_bd_vbase;
	dma_addr_t tx_bd_pbase;

	struct qtnf_rx_bd *rx_bd_vbase;
	dma_addr_t rx_bd_pbase;

	unsigned long bd_table_vaddr;
	dma_addr_t bd_table_paddr;
	u32 bd_table_len;

	u32 hw_txproc_wr_ptr;

	u16 tx_bd_reclaim_start;
	u16 tx_bd_index;
	u32 tx_queue_len;

	u16 rx_bd_index;

	u32 pcie_irq_mask;

	/* diagnostics stats */
	u32 pcie_irq_count;
	u32 tx_full_count;
};

/* alignment helper functions */

static __always_inline unsigned long
align_up_off(unsigned long val, unsigned long step)
{
	return (((val + (step - 1)) & (~(step - 1))) - val);
}

static __always_inline unsigned long
align_down_off(unsigned long val, unsigned long step)
{
	return ((val) & ((step) - 1));
}

static __always_inline unsigned long
align_val_up(unsigned long val, unsigned long step)
{
	return ((val + step - 1) & (~(step - 1)));
}

static __always_inline unsigned long
align_val_down(unsigned long val, unsigned long step)
{
	return (val & (~(step - 1)));
}

static __always_inline void *
align_buf_dma(void *addr)
{
	return (void *)align_val_up((unsigned long)addr,
				    dma_get_cache_alignment());
}

static __always_inline unsigned long
align_buf_dma_offset(void *addr)
{
	return (align_buf_dma(addr) - addr);
}

static __always_inline void *
align_buf_cache(void *addr)
{
	return (void *)align_val_down((unsigned long)addr,
				      dma_get_cache_alignment());
}

static __always_inline unsigned long
align_buf_cache_offset(void *addr)
{
	return (addr - align_buf_cache(addr));
}

static __always_inline unsigned long
align_buf_cache_size(void *addr, unsigned long size)
{
	return align_val_up(size + align_buf_cache_offset(addr),
			    dma_get_cache_alignment());
}

#endif /* _QTN_FMAC_PCIE_H_ */
