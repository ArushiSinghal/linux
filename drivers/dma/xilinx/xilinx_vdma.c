/*
 * DMA driver for Xilinx Video DMA Engine
 *
 * Copyright (C) 2010-2014 Xilinx, Inc. All rights reserved.
 *
 * Based on the Freescale DMA driver.
 *
 * Description:
 * The AXI Video Direct Memory Access (AXI VDMA) core is a soft Xilinx IP
 * core that provides high-bandwidth direct memory access between memory
 * and AXI4-Stream type video target peripherals. The core provides efficient
 * two dimensional DMA operations with independent asynchronous read (S2MM)
 * and write (MM2S) channel operation. It can be configured to have either
 * one channel or two channels. If configured as two channels, one is to
 * transmit to the video device (MM2S) and another is to receive from the
 * video device (S2MM). Initialization, status, interrupt and management
 * registers are accessed through an AXI4-Lite slave interface.
 *
 *  The AXI DMA, is a soft IP, which provides high-bandwidth Direct Memory
 *  Access between memory and AXI4-Stream-type target peripherals. It can be
 *  configured to have one channel or two channels and if configured as two
 *  channels, one is to transmit data from memory to a device and another is
 *  to receive from a device.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/bitops.h>
#include <linux/dmapool.h>
#include <linux/dma/xilinx_dma.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/slab.h>

#include "../dmaengine.h"

/* Register/Descriptor Offsets */
#define XILINX_VDMA_MM2S_CTRL_OFFSET		0x0000
#define XILINX_VDMA_S2MM_CTRL_OFFSET		0x0030
#define XILINX_VDMA_MM2S_DESC_OFFSET		0x0050
#define XILINX_VDMA_S2MM_DESC_OFFSET		0x00a0

/* Control Registers */
#define XILINX_VDMA_REG_DMACR			0x0000
#define XILINX_VDMA_DMACR_DELAY_MAX		0xff
#define XILINX_VDMA_DMACR_DELAY_SHIFT		24
#define XILINX_VDMA_DMACR_FRAME_COUNT_MAX	0xff
#define XILINX_VDMA_DMACR_FRAME_COUNT_SHIFT	16
#define XILINX_VDMA_DMACR_ERR_IRQ		BIT(14)
#define XILINX_VDMA_DMACR_DLY_CNT_IRQ		BIT(13)
#define XILINX_VDMA_DMACR_FRM_CNT_IRQ		BIT(12)
#define XILINX_VDMA_DMACR_MASTER_SHIFT		8
#define XILINX_VDMA_DMACR_FSYNCSRC_SHIFT	5
#define XILINX_VDMA_DMACR_FRAMECNT_EN		BIT(4)
#define XILINX_VDMA_DMACR_GENLOCK_EN		BIT(3)
#define XILINX_VDMA_DMACR_RESET			BIT(2)
#define XILINX_VDMA_DMACR_CIRC_EN		BIT(1)
#define XILINX_VDMA_DMACR_RUNSTOP		BIT(0)
#define XILINX_VDMA_DMACR_FSYNCSRC_MASK		GENMASK(6, 5)

#define XILINX_VDMA_REG_DMASR			0x0004
#define XILINX_VDMA_DMASR_EOL_LATE_ERR		BIT(15)
#define XILINX_VDMA_DMASR_ERR_IRQ		BIT(14)
#define XILINX_VDMA_DMASR_DLY_CNT_IRQ		BIT(13)
#define XILINX_VDMA_DMASR_FRM_CNT_IRQ		BIT(12)
#define XILINX_VDMA_DMASR_SOF_LATE_ERR		BIT(11)
#define XILINX_VDMA_DMASR_SG_DEC_ERR		BIT(10)
#define XILINX_VDMA_DMASR_SG_SLV_ERR		BIT(9)
#define XILINX_VDMA_DMASR_EOF_EARLY_ERR		BIT(8)
#define XILINX_VDMA_DMASR_SOF_EARLY_ERR		BIT(7)
#define XILINX_VDMA_DMASR_DMA_DEC_ERR		BIT(6)
#define XILINX_VDMA_DMASR_DMA_SLAVE_ERR		BIT(5)
#define XILINX_VDMA_DMASR_DMA_INT_ERR		BIT(4)
#define XILINX_VDMA_DMASR_IDLE			BIT(1)
#define XILINX_VDMA_DMASR_HALTED		BIT(0)
#define XILINX_VDMA_DMASR_DELAY_MASK		GENMASK(31, 24)
#define XILINX_VDMA_DMASR_FRAME_COUNT_MASK	GENMASK(23, 16)

#define XILINX_VDMA_REG_CURDESC			0x0008
#define XILINX_VDMA_REG_TAILDESC		0x0010
#define XILINX_VDMA_REG_REG_INDEX		0x0014
#define XILINX_VDMA_REG_FRMSTORE		0x0018
#define XILINX_VDMA_REG_THRESHOLD		0x001c
#define XILINX_VDMA_REG_FRMPTR_STS		0x0024
#define XILINX_VDMA_REG_PARK_PTR		0x0028
#define XILINX_VDMA_PARK_PTR_WR_REF_SHIFT	8
#define XILINX_VDMA_PARK_PTR_RD_REF_SHIFT	0
#define XILINX_VDMA_REG_VDMA_VERSION		0x002c

/* Register Direct Mode Registers */
#define XILINX_VDMA_REG_VSIZE			0x0000
#define XILINX_VDMA_REG_HSIZE			0x0004

#define XILINX_VDMA_REG_FRMDLY_STRIDE		0x0008
#define XILINX_VDMA_FRMDLY_STRIDE_FRMDLY_SHIFT	24
#define XILINX_VDMA_FRMDLY_STRIDE_STRIDE_SHIFT	0

#define XILINX_VDMA_REG_START_ADDRESS(n)	(0x000c + 4 * (n))

/* HW specific definitions */
#define XILINX_VDMA_MAX_CHANS_PER_DEVICE	0x2

#define XILINX_VDMA_DMAXR_ALL_IRQ_MASK	\
		(XILINX_VDMA_DMASR_FRM_CNT_IRQ | \
		 XILINX_VDMA_DMASR_DLY_CNT_IRQ | \
		 XILINX_VDMA_DMASR_ERR_IRQ)

#define XILINX_VDMA_DMASR_ALL_ERR_MASK	\
		(XILINX_VDMA_DMASR_EOL_LATE_ERR | \
		 XILINX_VDMA_DMASR_SOF_LATE_ERR | \
		 XILINX_VDMA_DMASR_SG_DEC_ERR | \
		 XILINX_VDMA_DMASR_SG_SLV_ERR | \
		 XILINX_VDMA_DMASR_EOF_EARLY_ERR | \
		 XILINX_VDMA_DMASR_SOF_EARLY_ERR | \
		 XILINX_VDMA_DMASR_DMA_DEC_ERR | \
		 XILINX_VDMA_DMASR_DMA_SLAVE_ERR | \
		 XILINX_VDMA_DMASR_DMA_INT_ERR)

/*
 * Recoverable errors are DMA Internal error, SOF Early, EOF Early
 * and SOF Late. They are only recoverable when C_FLUSH_ON_FSYNC
 * is enabled in the h/w system.
 */
#define XILINX_VDMA_DMASR_ERR_RECOVER_MASK	\
		(XILINX_VDMA_DMASR_SOF_LATE_ERR | \
		 XILINX_VDMA_DMASR_EOF_EARLY_ERR | \
		 XILINX_VDMA_DMASR_SOF_EARLY_ERR | \
		 XILINX_VDMA_DMASR_DMA_INT_ERR)

/* Axi VDMA Flush on Fsync bits */
#define XILINX_VDMA_FLUSH_S2MM		3
#define XILINX_VDMA_FLUSH_MM2S		2
#define XILINX_VDMA_FLUSH_BOTH		1

/* Delay loop counter to prevent hardware failure */
#define XILINX_VDMA_LOOP_COUNT		1000000

#define AXIVDMA_SUPPORT		BIT(0)
#define AXIDMA_SUPPORT		BIT(1)

/* AXI DMA Specific Registers/Offsets */
#define XILINX_DMA_REG_SRCDSTADDR	0x18
#define XILINX_DMA_REG_DSTADDR		0x20
#define XILINX_DMA_REG_BTT		0x28

#define XILINX_DMA_MAX_TRANS_LEN	GENMASK(22, 0)
#define XILINX_DMA_CR_COALESCE_MAX	GENMASK(23, 16)
#define XILINX_DMA_CR_COALESCE_SHIFT	16
#define XILINX_DMA_BD_SOP		BIT(27)
#define XILINX_DMA_BD_EOP		BIT(26)
#define XILINX_DMA_COALESCE_MAX		255
#define XILINX_DMA_NUM_APP_WORDS	5

/**
 * struct xilinx_vdma_desc_hw - Hardware Descriptor
 * @next_desc: Next Descriptor Pointer @0x00
 * @pad1: Reserved @0x04
 * @buf_addr: Buffer address @0x08
 * @pad2: Reserved @0x0C
 * @dstaddr_vsize: Vertical Size @0x10
 * @hsize: Horizontal Size @0x14
 * @control_stride: Number of bytes between the first
 *	    pixels of each horizontal line @0x18
 * @status: Status field @0x1C
 * @app: APP Fields @0x20 - 0x30
 */
struct xilinx_vdma_desc_hw {
	u32 next_desc;
	u32 pad1;
	u32 buf_addr;
	u32 pad2;
	u32 dstaddr_vsize;
	u32 hsize;
	u32 control_stride;
	u32 status;
	u32 app[XILINX_DMA_NUM_APP_WORDS];
} __aligned(64);

/**
 * struct xilinx_vdma_tx_segment - Descriptor segment
 * @hw: Hardware descriptor
 * @node: Node in the descriptor segments list
 * @phys: Physical address of segment
 */
struct xilinx_vdma_tx_segment {
	struct xilinx_vdma_desc_hw hw;
	struct list_head node;
	dma_addr_t phys;
} __aligned(64);

/**
 * struct xilinx_vdma_tx_descriptor - Per Transaction structure
 * @async_tx: Async transaction descriptor
 * @segments: TX segments list
 * @node: Node in the channel descriptors list
 */
struct xilinx_vdma_tx_descriptor {
	struct dma_async_tx_descriptor async_tx;
	struct list_head segments;
	struct list_head node;
};

/**
 * struct xilinx_vdma_chan - Driver specific VDMA channel structure
 * @xdev: Driver specific device structure
 * @ctrl_offset: Control registers offset
 * @desc_offset: TX descriptor registers offset
 * @lock: Descriptor operation lock
 * @pending_list: Descriptors waiting
 * @active_list: Descriptors ready to submit
 * @done_list: Complete descriptors
 * @common: DMA common channel
 * @desc_pool: Descriptors pool
 * @dev: The dma device
 * @irq: Channel IRQ
 * @id: Channel ID
 * @direction: Transfer direction
 * @num_frms: Number of frames
 * @has_sg: Support scatter transfers
 * @genlock: Support genlock mode
 * @err: Channel has errors
 * @tasklet: Cleanup work after irq
 * @config: Device configuration info
 * @flush_on_fsync: Flush on Frame sync
 * @desc_pendingcount: Descriptor pending count
 * @residue: Residue for AXI DMA
 * @seg_v: Statically allocated segments base
 * @start_transfer: Differentiate b/w DMA IP's transfer
 */
struct xilinx_vdma_chan {
	struct xilinx_vdma_device *xdev;
	u32 ctrl_offset;
	u32 desc_offset;
	spinlock_t lock;
	struct list_head pending_list;
	struct list_head active_list;
	struct list_head done_list;
	struct dma_chan common;
	struct dma_pool *desc_pool;
	struct device *dev;
	int irq;
	int id;
	enum dma_transfer_direction direction;
	int num_frms;
	bool has_sg;
	bool genlock;
	bool err;
	struct tasklet_struct tasklet;
	struct xilinx_vdma_config config;
	bool flush_on_fsync;
	u32 desc_pendingcount;
	u32 residue;
	struct xilinx_vdma_tx_segment *seg_v;
	void   (*start_transfer)(struct xilinx_vdma_chan *chan);
};

/**
 * struct xilinx_vdma_device - VDMA device structure
 * @regs: I/O mapped base address
 * @dev: Device Structure
 * @common: DMA device structure
 * @chan: Driver specific VDMA channel
 * @has_sg: Specifies whether Scatter-Gather is present or not
 * @flush_on_fsync: Flush on frame sync
 * @quirks: Needed for different IP cores
 */
struct xilinx_vdma_device {
	void __iomem *regs;
	struct device *dev;
	struct dma_device common;
	struct xilinx_vdma_chan *chan[XILINX_VDMA_MAX_CHANS_PER_DEVICE];
	bool has_sg;
	u32 flush_on_fsync;
	u32 quirks;
};

/**
 * struct xdma_platform_data - DMA platform structure
 * @quirks: quirks for platform specific data.
 */
struct xdma_platform_data {
	u32 quirks;
};

/* Macros */
#define to_xilinx_chan(chan) \
	container_of(chan, struct xilinx_vdma_chan, common)
#define to_vdma_tx_descriptor(tx) \
	container_of(tx, struct xilinx_vdma_tx_descriptor, async_tx)
#define xilinx_vdma_poll_timeout(chan, reg, val, cond, delay_us, timeout_us) \
	readl_poll_timeout(chan->xdev->regs + chan->ctrl_offset + reg, val, \
			   cond, delay_us, timeout_us)

/* IO accessors */
static inline u32 vdma_read(struct xilinx_vdma_chan *chan, u32 reg)
{
	return ioread32(chan->xdev->regs + reg);
}

static inline void vdma_write(struct xilinx_vdma_chan *chan, u32 reg, u32 value)
{
	iowrite32(value, chan->xdev->regs + reg);
}

static inline void vdma_desc_write(struct xilinx_vdma_chan *chan, u32 reg,
				   u32 value)
{
	vdma_write(chan, chan->desc_offset + reg, value);
}

static inline u32 vdma_ctrl_read(struct xilinx_vdma_chan *chan, u32 reg)
{
	return vdma_read(chan, chan->ctrl_offset + reg);
}

static inline void vdma_ctrl_write(struct xilinx_vdma_chan *chan, u32 reg,
				   u32 value)
{
	vdma_write(chan, chan->ctrl_offset + reg, value);
}

static inline void vdma_ctrl_clr(struct xilinx_vdma_chan *chan, u32 reg,
				 u32 clr)
{
	vdma_ctrl_write(chan, reg, vdma_ctrl_read(chan, reg) & ~clr);
}

static inline void vdma_ctrl_set(struct xilinx_vdma_chan *chan, u32 reg,
				 u32 set)
{
	vdma_ctrl_write(chan, reg, vdma_ctrl_read(chan, reg) | set);
}

/* -----------------------------------------------------------------------------
 * Descriptors and segments alloc and free
 */

/**
 * xilinx_vdma_alloc_tx_segment - Allocate transaction segment
 * @chan: Driver specific VDMA channel
 *
 * Return: The allocated segment on success and NULL on failure.
 */
static struct xilinx_vdma_tx_segment *
xilinx_vdma_alloc_tx_segment(struct xilinx_vdma_chan *chan)
{
	struct xilinx_vdma_tx_segment *segment;
	dma_addr_t phys;

	segment = dma_pool_alloc(chan->desc_pool, GFP_ATOMIC, &phys);
	if (!segment)
		return NULL;

	memset(segment, 0, sizeof(*segment));
	segment->phys = phys;

	return segment;
}

/**
 * xilinx_vdma_free_tx_segment - Free transaction segment
 * @chan: Driver specific VDMA channel
 * @segment: VDMA transaction segment
 */
static void xilinx_vdma_free_tx_segment(struct xilinx_vdma_chan *chan,
					struct xilinx_vdma_tx_segment *segment)
{
	dma_pool_free(chan->desc_pool, segment, segment->phys);
}

/**
 * xilinx_vdma_tx_descriptor - Allocate transaction descriptor
 * @chan: Driver specific VDMA channel
 *
 * Return: The allocated descriptor on success and NULL on failure.
 */
static struct xilinx_vdma_tx_descriptor *
xilinx_vdma_alloc_tx_descriptor(struct xilinx_vdma_chan *chan)
{
	struct xilinx_vdma_tx_descriptor *desc;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return NULL;

	INIT_LIST_HEAD(&desc->segments);

	return desc;
}

/**
 * xilinx_vdma_free_tx_descriptor - Free transaction descriptor
 * @chan: Driver specific VDMA channel
 * @desc: VDMA transaction descriptor
 */
static void
xilinx_vdma_free_tx_descriptor(struct xilinx_vdma_chan *chan,
			       struct xilinx_vdma_tx_descriptor *desc)
{
	struct xilinx_vdma_tx_segment *segment, *next;

	if (!desc)
		return;

	list_for_each_entry_safe(segment, next, &desc->segments, node) {
		list_del(&segment->node);
		xilinx_vdma_free_tx_segment(chan, segment);
	}

	kfree(desc);
}

/* Required functions */

/**
 * xilinx_vdma_free_desc_list - Free descriptors list
 * @chan: Driver specific VDMA channel
 * @list: List to parse and delete the descriptor
 */
static void xilinx_vdma_free_desc_list(struct xilinx_vdma_chan *chan,
					struct list_head *list)
{
	struct xilinx_vdma_tx_descriptor *desc, *next;

	list_for_each_entry_safe(desc, next, list, node) {
		list_del(&desc->node);
		xilinx_vdma_free_tx_descriptor(chan, desc);
	}
}

/**
 * xilinx_vdma_free_descriptors - Free channel descriptors
 * @chan: Driver specific VDMA channel
 */
static void xilinx_vdma_free_descriptors(struct xilinx_vdma_chan *chan)
{
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);

	xilinx_vdma_free_desc_list(chan, &chan->pending_list);
	xilinx_vdma_free_desc_list(chan, &chan->done_list);
	xilinx_vdma_free_desc_list(chan, &chan->active_list);

	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_vdma_free_chan_resources - Free channel resources
 * @dchan: DMA channel
 */
static void xilinx_vdma_free_chan_resources(struct dma_chan *dchan)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);

	dev_dbg(chan->dev, "Free all channel resources.\n");

	xilinx_vdma_free_descriptors(chan);
	xilinx_vdma_free_tx_segment(chan, chan->seg_v);
	dma_pool_destroy(chan->desc_pool);
	chan->desc_pool = NULL;
}

/**
 * xilinx_vdma_chan_desc_cleanup - Clean channel descriptors
 * @chan: Driver specific VDMA channel
 */
static void xilinx_vdma_chan_desc_cleanup(struct xilinx_vdma_chan *chan)
{
	struct xilinx_vdma_tx_descriptor *desc, *next;
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);

	list_for_each_entry_safe(desc, next, &chan->done_list, node) {
		dma_async_tx_callback callback;
		void *callback_param;

		/* Remove from the list of running transactions */
		list_del(&desc->node);

		/* Run the link descriptor callback function */
		callback = desc->async_tx.callback;
		callback_param = desc->async_tx.callback_param;
		if (callback) {
			spin_unlock_irqrestore(&chan->lock, flags);
			callback(callback_param);
			spin_lock_irqsave(&chan->lock, flags);
		}

		/* Run any dependencies, then free the descriptor */
		dma_run_dependencies(&desc->async_tx);
		xilinx_vdma_free_tx_descriptor(chan, desc);
	}

	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_vdma_do_tasklet - Schedule completion tasklet
 * @data: Pointer to the Xilinx VDMA channel structure
 */
static void xilinx_vdma_do_tasklet(unsigned long data)
{
	struct xilinx_vdma_chan *chan = (struct xilinx_vdma_chan *)data;

	xilinx_vdma_chan_desc_cleanup(chan);
}

/**
 * xilinx_vdma_alloc_chan_resources - Allocate channel resources
 * @dchan: DMA channel
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_vdma_alloc_chan_resources(struct dma_chan *dchan)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);

	/* Has this channel already been allocated? */
	if (chan->desc_pool)
		return 0;

	/*
	 * We need the descriptor to be aligned to 64bytes
	 * for meeting Xilinx VDMA specification requirement.
	 */
	chan->desc_pool = dma_pool_create("xilinx_vdma_desc_pool",
				chan->dev,
				sizeof(struct xilinx_vdma_tx_segment),
				__alignof__(struct xilinx_vdma_tx_segment), 0);
	if (!chan->desc_pool) {
		dev_err(chan->dev,
			"unable to allocate channel %d descriptor pool\n",
			chan->id);
		return -ENOMEM;
	}

	chan->seg_v = xilinx_vdma_alloc_tx_segment(chan);
	dma_cookie_init(dchan);

	/* Enable interrupts */
	vdma_ctrl_set(chan, XILINX_VDMA_REG_DMACR,
		      XILINX_VDMA_DMAXR_ALL_IRQ_MASK);
	return 0;
}

/**
 * xilinx_vdma_tx_status - Get VDMA transaction status
 * @dchan: DMA channel
 * @cookie: Transaction identifier
 * @txstate: Transaction state
 *
 * Return: DMA transaction status
 */
static enum dma_status xilinx_vdma_tx_status(struct dma_chan *dchan,
					dma_cookie_t cookie,
					struct dma_tx_state *txstate)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);
	struct xilinx_vdma_tx_descriptor *desc;
	struct xilinx_vdma_tx_segment *segment;
	struct xilinx_vdma_desc_hw *hw;
	enum dma_status ret;
	unsigned long flags;
	u32 residue = 0;

	ret = dma_cookie_status(dchan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	if (chan->xdev->quirks & AXIDMA_SUPPORT) {
		desc = list_last_entry(&chan->active_list,
				       struct xilinx_vdma_tx_descriptor, node);

		spin_lock_irqsave(&chan->lock, flags);
		if (chan->has_sg) {
			list_for_each_entry(segment, &desc->segments, node) {
				hw = &segment->hw;
				residue += (hw->control_stride - hw->status) &
					   XILINX_DMA_MAX_TRANS_LEN;
			}
		}
		spin_unlock_irqrestore(&chan->lock, flags);

		chan->residue = residue;
		dma_set_residue(txstate, chan->residue);
	}

	return ret;
}

/**
 * xilinx_vdma_is_running - Check if VDMA channel is running
 * @chan: Driver specific VDMA channel
 *
 * Return: '1' if running, '0' if not.
 */
static bool xilinx_vdma_is_running(struct xilinx_vdma_chan *chan)
{
	return !(vdma_ctrl_read(chan, XILINX_VDMA_REG_DMASR) &
		 XILINX_VDMA_DMASR_HALTED) &&
		(vdma_ctrl_read(chan, XILINX_VDMA_REG_DMACR) &
		 XILINX_VDMA_DMACR_RUNSTOP);
}

/**
 * xilinx_vdma_is_idle - Check if VDMA channel is idle
 * @chan: Driver specific VDMA channel
 *
 * Return: '1' if idle, '0' if not.
 */
static bool xilinx_vdma_is_idle(struct xilinx_vdma_chan *chan)
{
	return vdma_ctrl_read(chan, XILINX_VDMA_REG_DMASR) &
		XILINX_VDMA_DMASR_IDLE;
}

/**
 * xilinx_vdma_halt - Halt VDMA channel
 * @chan: Driver specific VDMA channel
 */
static void xilinx_vdma_halt(struct xilinx_vdma_chan *chan)
{
	int err;
	u32 val;

	vdma_ctrl_clr(chan, XILINX_VDMA_REG_DMACR, XILINX_VDMA_DMACR_RUNSTOP);

	/* Wait for the hardware to halt */
	err = xilinx_vdma_poll_timeout(chan, XILINX_VDMA_REG_DMASR, val,
				      (val & XILINX_VDMA_DMASR_HALTED), 0,
				      XILINX_VDMA_LOOP_COUNT);

	if (err) {
		dev_err(chan->dev, "Cannot stop channel %p: %x\n",
			chan, vdma_ctrl_read(chan, XILINX_VDMA_REG_DMASR));
		chan->err = true;
	}
}

/**
 * xilinx_vdma_start - Start VDMA channel
 * @chan: Driver specific VDMA channel
 */
static void xilinx_vdma_start(struct xilinx_vdma_chan *chan)
{
	int err;
	u32 val;

	vdma_ctrl_set(chan, XILINX_VDMA_REG_DMACR, XILINX_VDMA_DMACR_RUNSTOP);

	/* Wait for the hardware to start */
	err = xilinx_vdma_poll_timeout(chan, XILINX_VDMA_REG_DMASR, val,
				      !(val & XILINX_VDMA_DMASR_HALTED), 0,
				      XILINX_VDMA_LOOP_COUNT);

	if (err) {
		dev_err(chan->dev, "Cannot start channel %p: %x\n",
			chan, vdma_ctrl_read(chan, XILINX_VDMA_REG_DMASR));

		chan->err = true;
	}
}

/**
 * xilinx_vdma_start_transfer - Starts VDMA transfer
 * @chan: Driver specific channel struct pointer
 */
static void xilinx_vdma_start_transfer(struct xilinx_vdma_chan *chan)
{
	struct xilinx_vdma_config *config = &chan->config;
	struct xilinx_vdma_tx_descriptor *desc, *tail_desc;
	u32 reg;
	struct xilinx_vdma_tx_segment *tail_segment;

	/* This function was invoked with lock held */
	if (chan->err)
		return;

	if (list_empty(&chan->pending_list))
		return;

	desc = list_first_entry(&chan->pending_list,
				struct xilinx_vdma_tx_descriptor, node);
	tail_desc = list_last_entry(&chan->pending_list,
				    struct xilinx_vdma_tx_descriptor, node);

	tail_segment = list_last_entry(&tail_desc->segments,
				       struct xilinx_vdma_tx_segment, node);

	/* If it is SG mode and hardware is busy, cannot submit */
	if (chan->has_sg && xilinx_vdma_is_running(chan) &&
	    !xilinx_vdma_is_idle(chan)) {
		dev_dbg(chan->dev, "DMA controller still busy\n");
		return;
	}

	/*
	 * If hardware is idle, then all descriptors on the running lists are
	 * done, start new transfers
	 */
	if (chan->has_sg)
		vdma_ctrl_write(chan, XILINX_VDMA_REG_CURDESC,
				desc->async_tx.phys);

	/* Configure the hardware using info in the config structure */
	reg = vdma_ctrl_read(chan, XILINX_VDMA_REG_DMACR);

	if (config->frm_cnt_en)
		reg |= XILINX_VDMA_DMACR_FRAMECNT_EN;
	else
		reg &= ~XILINX_VDMA_DMACR_FRAMECNT_EN;

	/* Configure channel to allow number frame buffers */
	vdma_ctrl_write(chan, XILINX_VDMA_REG_FRMSTORE,
			chan->desc_pendingcount);

	/*
	 * With SG, start with circular mode, so that BDs can be fetched.
	 * In direct register mode, if not parking, enable circular mode
	 */
	if (chan->has_sg || !config->park)
		reg |= XILINX_VDMA_DMACR_CIRC_EN;

	if (config->park)
		reg &= ~XILINX_VDMA_DMACR_CIRC_EN;

	vdma_ctrl_write(chan, XILINX_VDMA_REG_DMACR, reg);

	if (config->park && (config->park_frm >= 0) &&
			(config->park_frm < chan->num_frms)) {
		if (chan->direction == DMA_MEM_TO_DEV)
			vdma_write(chan, XILINX_VDMA_REG_PARK_PTR,
				config->park_frm <<
					XILINX_VDMA_PARK_PTR_RD_REF_SHIFT);
		else
			vdma_write(chan, XILINX_VDMA_REG_PARK_PTR,
				config->park_frm <<
					XILINX_VDMA_PARK_PTR_WR_REF_SHIFT);
	}

	/* Start the hardware */
	xilinx_vdma_start(chan);

	if (chan->err)
		return;

	/* Start the transfer */
	if (chan->has_sg) {
		vdma_ctrl_write(chan, XILINX_VDMA_REG_TAILDESC,
				tail_segment->phys);
	} else {
		struct xilinx_vdma_tx_segment *segment, *last = NULL;
		int i = 0;

		list_for_each_entry(desc, &chan->pending_list, node) {
			segment = list_first_entry(&desc->segments,
					   struct xilinx_vdma_tx_segment, node);
			vdma_desc_write(chan,
					XILINX_VDMA_REG_START_ADDRESS(i++),
					segment->hw.buf_addr);
			last = segment;
		}

		if (!last)
			return;

		/* HW expects these parameters to be same for one transaction */
		vdma_desc_write(chan, XILINX_VDMA_REG_HSIZE, last->hw.hsize);
		vdma_desc_write(chan, XILINX_VDMA_REG_FRMDLY_STRIDE,
				last->hw.control_stride);
		vdma_desc_write(chan, XILINX_VDMA_REG_VSIZE,
				last->hw.dstaddr_vsize);
	}

	list_splice_tail_init(&chan->pending_list, &chan->active_list);
	chan->desc_pendingcount = 0;
}

/**
 * xilinx_vdma_issue_pending - Issue pending transactions
 * @dchan: DMA channel
 */
static void xilinx_vdma_issue_pending(struct dma_chan *dchan)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);
	xilinx_vdma_start_transfer(chan);
	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_dma_start_transfer - Starts DMA transfer
 * @chan: Driver specific channel struct pointer
 */
static void xilinx_dma_start_transfer(struct xilinx_vdma_chan *chan)
{
	struct xilinx_vdma_tx_descriptor *head_desc, *tail_desc;
	struct xilinx_vdma_tx_segment *tail_segment, *old_head, *new_head;
	u32 reg;

	/* This function was invoked with lock held */
	if (chan->err)
		return;

	if (list_empty(&chan->pending_list))
		return;

	head_desc = list_first_entry(&chan->pending_list,
				     struct xilinx_vdma_tx_descriptor, node);
	tail_desc = list_last_entry(&chan->pending_list,
				    struct xilinx_vdma_tx_descriptor, node);
	tail_segment = list_last_entry(&tail_desc->segments,
				       struct xilinx_vdma_tx_segment, node);

	old_head = list_first_entry(&head_desc->segments,
				struct xilinx_vdma_tx_segment, node);
	new_head = chan->seg_v;
	/* Copy Buffer Descriptor fields. */
	new_head->hw = old_head->hw;

	/* Swap and save new reserve */
	list_replace_init(&old_head->node, &new_head->node);
	chan->seg_v = old_head;

	tail_segment->hw.next_desc = chan->seg_v->phys;
	head_desc->async_tx.phys = new_head->phys;

	/* If it is SG mode and hardware is busy, cannot submit */
	if (chan->has_sg && xilinx_vdma_is_running(chan) &&
	    !xilinx_vdma_is_idle(chan)) {
		dev_dbg(chan->dev, "DMA controller still busy\n");
		return;
	}

	reg = vdma_ctrl_read(chan, XILINX_VDMA_REG_DMACR);

	if (chan->desc_pendingcount <= XILINX_DMA_COALESCE_MAX) {
		reg &= ~XILINX_DMA_CR_COALESCE_MAX;
		reg |= chan->desc_pendingcount <<
				  XILINX_DMA_CR_COALESCE_SHIFT;
		vdma_ctrl_write(chan, XILINX_VDMA_REG_DMACR, reg);
	}

	if (chan->has_sg)
		vdma_ctrl_write(chan, XILINX_VDMA_REG_CURDESC,
				head_desc->async_tx.phys);

	xilinx_vdma_start(chan);

	if (chan->err)
		return;

	/* Start the transfer */
	if (chan->has_sg) {
		vdma_ctrl_write(chan, XILINX_VDMA_REG_TAILDESC,
			       tail_segment->phys);
	} else {
		struct xilinx_vdma_tx_segment *segment;
		struct xilinx_vdma_desc_hw *hw;

		segment = list_first_entry(&head_desc->segments,
					   struct xilinx_vdma_tx_segment, node);
		hw = &segment->hw;

		vdma_ctrl_write(chan, XILINX_DMA_REG_SRCDSTADDR, hw->buf_addr);

		/* Start the transfer */
		vdma_ctrl_write(chan, XILINX_DMA_REG_BTT,
			       hw->control_stride & XILINX_DMA_MAX_TRANS_LEN);
	}

	list_splice_tail_init(&chan->pending_list, &chan->active_list);
	chan->desc_pendingcount = 0;
}

/**
 * xilinx_dma_issue_pending - Issue pending transactions
 * @dchan: DMA channel
 */
static void xilinx_dma_issue_pending(struct dma_chan *dchan)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);
	xilinx_dma_start_transfer(chan);
	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_vdma_complete_descriptor - Mark the active descriptor as complete
 * @chan : xilinx DMA channel
 *
 * CONTEXT: hardirq
 */
static void xilinx_vdma_complete_descriptor(struct xilinx_vdma_chan *chan)
{
	struct xilinx_vdma_tx_descriptor *desc, *next;

	/* This function was invoked with lock held */
	if (list_empty(&chan->active_list))
		return;

	list_for_each_entry_safe(desc, next, &chan->active_list, node) {
		list_del(&desc->node);
		dma_cookie_complete(&desc->async_tx);
		list_add_tail(&desc->node, &chan->done_list);
	}
}

/**
 * xilinx_vdma_reset - Reset VDMA channel
 * @chan: Driver specific VDMA channel
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_vdma_reset(struct xilinx_vdma_chan *chan)
{
	int err;
	u32 tmp;

	vdma_ctrl_set(chan, XILINX_VDMA_REG_DMACR, XILINX_VDMA_DMACR_RESET);

	/* Wait for the hardware to finish reset */
	err = xilinx_vdma_poll_timeout(chan, XILINX_VDMA_REG_DMACR, tmp,
				      !(tmp & XILINX_VDMA_DMACR_RESET), 0,
				      XILINX_VDMA_LOOP_COUNT);

	if (err) {
		dev_err(chan->dev, "reset timeout, cr %x, sr %x\n",
			vdma_ctrl_read(chan, XILINX_VDMA_REG_DMACR),
			vdma_ctrl_read(chan, XILINX_VDMA_REG_DMASR));
		return -ETIMEDOUT;
	}

	chan->err = false;

	return err;
}

/**
 * xilinx_vdma_chan_reset - Reset VDMA channel and enable interrupts
 * @chan: Driver specific VDMA channel
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_vdma_chan_reset(struct xilinx_vdma_chan *chan)
{
	int err;

	/* Reset VDMA */
	err = xilinx_vdma_reset(chan);
	if (err)
		return err;

	/* Enable interrupts */
	vdma_ctrl_set(chan, XILINX_VDMA_REG_DMACR,
		      XILINX_VDMA_DMAXR_ALL_IRQ_MASK);

	return 0;
}

/**
 * xilinx_vdma_irq_handler - VDMA Interrupt handler
 * @irq: IRQ number
 * @data: Pointer to the Xilinx VDMA channel structure
 *
 * Return: IRQ_HANDLED/IRQ_NONE
 */
static irqreturn_t xilinx_vdma_irq_handler(int irq, void *data)
{
	struct xilinx_vdma_chan *chan = data;
	u32 status;

	/* Read the status and ack the interrupts. */
	status = vdma_ctrl_read(chan, XILINX_VDMA_REG_DMASR);
	if (!(status & XILINX_VDMA_DMAXR_ALL_IRQ_MASK))
		return IRQ_NONE;

	vdma_ctrl_write(chan, XILINX_VDMA_REG_DMASR,
			status & XILINX_VDMA_DMAXR_ALL_IRQ_MASK);

	if (status & XILINX_VDMA_DMASR_ERR_IRQ) {
		/*
		 * An error occurred. If C_FLUSH_ON_FSYNC is enabled and the
		 * error is recoverable, ignore it. Otherwise flag the error.
		 *
		 * Only recoverable errors can be cleared in the DMASR register,
		 * make sure not to write to other error bits to 1.
		 */
		u32 errors = status & XILINX_VDMA_DMASR_ALL_ERR_MASK;

		vdma_ctrl_write(chan, XILINX_VDMA_REG_DMASR,
				errors & XILINX_VDMA_DMASR_ERR_RECOVER_MASK);

		dev_err(chan->dev,
			"Channel %p has errors %x, cdr %x tdr %x\n",
			chan, errors,
			vdma_ctrl_read(chan, XILINX_VDMA_REG_CURDESC),
			vdma_ctrl_read(chan, XILINX_VDMA_REG_TAILDESC));
		chan->err = true;
	}

	if (status & XILINX_VDMA_DMASR_DLY_CNT_IRQ) {
		/*
		 * Device takes too long to do the transfer when user requires
		 * responsiveness.
		 */
		dev_dbg(chan->dev, "Inter-packet latency too long\n");
	}

	if (status & XILINX_VDMA_DMASR_FRM_CNT_IRQ) {
		spin_lock(&chan->lock);
		xilinx_vdma_complete_descriptor(chan);
		chan->start_transfer(chan);
		spin_unlock(&chan->lock);
	}

	tasklet_schedule(&chan->tasklet);
	return IRQ_HANDLED;
}

/**
 * append_desc_queue - Queuing descriptor
 * @chan: Driver specific dma channel
 * @desc: dma transaction descriptor
 */
static void append_desc_queue(struct xilinx_vdma_chan *chan,
			      struct xilinx_vdma_tx_descriptor *desc)
{
	struct xilinx_vdma_tx_segment *tail_segment;
	struct xilinx_vdma_tx_descriptor *tail_desc;

	if (list_empty(&chan->pending_list))
		goto append;

	/*
	 * Add the hardware descriptor to the chain of hardware descriptors
	 * that already exists in memory.
	 */
	tail_desc = list_last_entry(&chan->pending_list,
				    struct xilinx_vdma_tx_descriptor, node);
	tail_segment = list_last_entry(&tail_desc->segments,
				       struct xilinx_vdma_tx_segment, node);
	tail_segment->hw.next_desc = (u32)desc->async_tx.phys;

	/*
	 * Add the software descriptor and all children to the list
	 * of pending transactions
	 */
append:
	list_add_tail(&desc->node, &chan->pending_list);
	chan->desc_pendingcount++;

	if ((unlikely(chan->desc_pendingcount > chan->num_frms)) &&
		(chan->xdev->quirks & AXIVDMA_SUPPORT)) {
		dev_dbg(chan->dev, "desc pendingcount is too high\n");
		chan->desc_pendingcount = chan->num_frms;
	}
}

/**
 * xilinx_vdma_tx_submit - Submit DMA transaction
 * @tx: Async transaction descriptor
 *
 * Return: cookie value on success and failure value on error
 */
static dma_cookie_t xilinx_vdma_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct xilinx_vdma_tx_descriptor *desc = to_vdma_tx_descriptor(tx);
	struct xilinx_vdma_chan *chan = to_xilinx_chan(tx->chan);
	dma_cookie_t cookie;
	unsigned long flags;
	int err;

	if (chan->err) {
		/*
		 * If reset fails, need to hard reset the system.
		 * Channel is no longer functional
		 */
		err = xilinx_vdma_chan_reset(chan);
		if (err < 0)
			return err;
	}

	spin_lock_irqsave(&chan->lock, flags);

	cookie = dma_cookie_assign(tx);

	/* Put this transaction onto the tail of the pending queue */
	append_desc_queue(chan, desc);

	spin_unlock_irqrestore(&chan->lock, flags);

	return cookie;
}

/**
 * xilinx_vdma_dma_prep_interleaved - prepare a descriptor for a
 *	DMA_SLAVE transaction
 * @dchan: DMA channel
 * @xt: Interleaved template pointer
 * @flags: transfer ack flags
 *
 * Return: Async transaction descriptor on success and NULL on failure
 */
static struct dma_async_tx_descriptor *
xilinx_vdma_dma_prep_interleaved(struct dma_chan *dchan,
				 struct dma_interleaved_template *xt,
				 unsigned long flags)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);
	struct xilinx_vdma_tx_descriptor *desc;
	struct xilinx_vdma_tx_segment *segment, *prev = NULL;
	struct xilinx_vdma_desc_hw *hw;

	if (!is_slave_direction(xt->dir))
		return NULL;

	if (!xt->numf || !xt->sgl[0].size)
		return NULL;

	if (xt->frame_size != 1)
		return NULL;

	/* Allocate a transaction descriptor. */
	desc = xilinx_vdma_alloc_tx_descriptor(chan);
	if (!desc)
		return NULL;

	dma_async_tx_descriptor_init(&desc->async_tx, &chan->common);
	desc->async_tx.tx_submit = xilinx_vdma_tx_submit;
	async_tx_ack(&desc->async_tx);

	/* Allocate the link descriptor from DMA pool */
	segment = xilinx_vdma_alloc_tx_segment(chan);
	if (!segment)
		goto error;

	/* Fill in the hardware descriptor */
	hw = &segment->hw;
	hw->dstaddr_vsize = xt->numf;
	hw->hsize = xt->sgl[0].size;
	hw->control_stride = (xt->sgl[0].icg + xt->sgl[0].size) <<
			XILINX_VDMA_FRMDLY_STRIDE_STRIDE_SHIFT;
	hw->control_stride |= chan->config.frm_dly <<
			XILINX_VDMA_FRMDLY_STRIDE_FRMDLY_SHIFT;

	if (xt->dir != DMA_MEM_TO_DEV)
		hw->buf_addr = xt->dst_start;
	else
		hw->buf_addr = xt->src_start;

	/* Insert the segment into the descriptor segments list. */
	list_add_tail(&segment->node, &desc->segments);

	prev = segment;

	/* Link the last hardware descriptor with the first. */
	segment = list_first_entry(&desc->segments,
				   struct xilinx_vdma_tx_segment, node);
	desc->async_tx.phys = segment->phys;

	return &desc->async_tx;

error:
	xilinx_vdma_free_tx_descriptor(chan, desc);
	return NULL;
}

/**
 * xilinx_dma_prep_slave_sg - prepare descriptors for a DMA_SLAVE transaction
 * @dchan: DMA channel
 * @sgl: scatterlist to transfer to/from
 * @sg_len: number of entries in @scatterlist
 * @direction: DMA direction
 * @flags: transfer ack flags
 * @context: APP words of the descriptor
 *
 * Return: Async transaction descriptor on success and NULL on failure
 */
static struct dma_async_tx_descriptor *xilinx_dma_prep_slave_sg(
	struct dma_chan *dchan, struct scatterlist *sgl, unsigned int sg_len,
	enum dma_transfer_direction direction, unsigned long flags,
	void *context)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);
	struct xilinx_vdma_tx_descriptor *desc;
	struct xilinx_vdma_tx_segment *segment = NULL, *prev = NULL;
	u32 *app_w = (u32 *)context;
	struct scatterlist *sg;
	size_t copy, sg_used;
	int i;

	if (!is_slave_direction(direction))
		return NULL;

	/* Allocate a transaction descriptor. */
	desc = xilinx_vdma_alloc_tx_descriptor(chan);
	if (!desc)
		return NULL;

	dma_async_tx_descriptor_init(&desc->async_tx, &chan->common);
	desc->async_tx.tx_submit = xilinx_vdma_tx_submit;

	/* Build transactions using information in the scatter gather list */
	for_each_sg(sgl, sg, sg_len, i) {
		sg_used = 0;

		/* Loop until the entire scatterlist entry is used */
		while (sg_used < sg_dma_len(sg)) {
			struct xilinx_vdma_desc_hw *hw;

			/* Get a free segment */
			segment = xilinx_vdma_alloc_tx_segment(chan);
			if (!segment)
				goto error;

			/*
			 * Calculate the maximum number of bytes to transfer,
			 * making sure it is less than the hw limit
			 */
			copy = min_t(size_t, sg_dma_len(sg) - sg_used,
				     XILINX_DMA_MAX_TRANS_LEN);
			hw = &segment->hw;

			/* Fill in the descriptor */
			hw->buf_addr = sg_dma_address(sg) + sg_used;

			hw->control_stride = copy;

			if (chan->direction == DMA_MEM_TO_DEV) {
				if (app_w)
					memcpy(hw->app, app_w, sizeof(u32) *
					       XILINX_DMA_NUM_APP_WORDS);
			}

			if (prev)
				prev->hw.next_desc = segment->phys;

			prev = segment;
			sg_used += copy;

			/*
			 * Insert the segment into the descriptor segments
			 * list.
			 */
			list_add_tail(&segment->node, &desc->segments);
		}
	}

	segment = list_first_entry(&desc->segments,
				   struct xilinx_vdma_tx_segment, node);
	desc->async_tx.phys = segment->phys;
	prev->hw.next_desc = segment->phys;

	/* For the last DMA_MEM_TO_DEV transfer, set EOP */
	if (chan->direction == DMA_MEM_TO_DEV) {
		segment->hw.control_stride |= XILINX_DMA_BD_SOP;
		segment = list_last_entry(&desc->segments,
					  struct xilinx_vdma_tx_segment,
					  node);
		segment->hw.control_stride |= XILINX_DMA_BD_EOP;
	}

	return &desc->async_tx;

error:
	xilinx_vdma_free_tx_descriptor(chan, desc);
	return NULL;
}

/**
 * xilinx_vdma_terminate_all - Halt the channel and free descriptors
 * @chan: Driver specific VDMA Channel pointer
 */
static int xilinx_vdma_terminate_all(struct dma_chan *dchan)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);

	/* Halt the DMA engine */
	xilinx_vdma_halt(chan);

	/* Remove and free all of the descriptors in the lists */
	xilinx_vdma_free_descriptors(chan);

	return 0;
}

/**
 * xilinx_vdma_channel_set_config - Configure VDMA channel
 * Run-time configuration for Axi VDMA, supports:
 * . halt the channel
 * . configure interrupt coalescing and inter-packet delay threshold
 * . start/stop parking
 * . enable genlock
 *
 * @dchan: DMA channel
 * @cfg: VDMA device configuration pointer
 *
 * Return: '0' on success and failure value on error
 */
int xilinx_vdma_channel_set_config(struct dma_chan *dchan,
					struct xilinx_vdma_config *cfg)
{
	struct xilinx_vdma_chan *chan = to_xilinx_chan(dchan);
	u32 dmacr;

	if (cfg->reset)
		return xilinx_vdma_chan_reset(chan);

	dmacr = vdma_ctrl_read(chan, XILINX_VDMA_REG_DMACR);

	chan->config.frm_dly = cfg->frm_dly;
	chan->config.park = cfg->park;

	/* genlock settings */
	chan->config.gen_lock = cfg->gen_lock;
	chan->config.master = cfg->master;

	if (cfg->gen_lock && chan->genlock) {
		dmacr |= XILINX_VDMA_DMACR_GENLOCK_EN;
		dmacr |= cfg->master << XILINX_VDMA_DMACR_MASTER_SHIFT;
	}

	chan->config.frm_cnt_en = cfg->frm_cnt_en;
	if (cfg->park)
		chan->config.park_frm = cfg->park_frm;
	else
		chan->config.park_frm = -1;

	chan->config.coalesc = cfg->coalesc;
	chan->config.delay = cfg->delay;

	if (cfg->coalesc <= XILINX_VDMA_DMACR_FRAME_COUNT_MAX) {
		dmacr |= cfg->coalesc << XILINX_VDMA_DMACR_FRAME_COUNT_SHIFT;
		chan->config.coalesc = cfg->coalesc;
	}

	if (cfg->delay <= XILINX_VDMA_DMACR_DELAY_MAX) {
		dmacr |= cfg->delay << XILINX_VDMA_DMACR_DELAY_SHIFT;
		chan->config.delay = cfg->delay;
	}

	/* FSync Source selection */
	dmacr &= ~XILINX_VDMA_DMACR_FSYNCSRC_MASK;
	dmacr |= cfg->ext_fsync << XILINX_VDMA_DMACR_FSYNCSRC_SHIFT;

	vdma_ctrl_write(chan, XILINX_VDMA_REG_DMACR, dmacr);

	return 0;
}
EXPORT_SYMBOL(xilinx_vdma_channel_set_config);

/* -----------------------------------------------------------------------------
 * Probe and remove
 */

/**
 * xilinx_vdma_chan_remove - Per Channel remove function
 * @chan: Driver specific VDMA channel
 */
static void xilinx_vdma_chan_remove(struct xilinx_vdma_chan *chan)
{
	/* Disable all interrupts */
	vdma_ctrl_clr(chan, XILINX_VDMA_REG_DMACR,
		      XILINX_VDMA_DMAXR_ALL_IRQ_MASK);

	if (chan->irq > 0)
		free_irq(chan->irq, chan);

	tasklet_kill(&chan->tasklet);

	list_del(&chan->common.device_node);
}

/**
 * xilinx_vdma_chan_probe - Per Channel Probing
 * It get channel features from the device tree entry and
 * initialize special channel handling routines
 *
 * @xdev: Driver specific device structure
 * @node: Device node
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_vdma_chan_probe(struct xilinx_vdma_device *xdev,
				  struct device_node *node)
{
	struct xilinx_vdma_chan *chan;
	bool has_dre = false;
	u32 value, width;
	int err;

	/* Allocate and initialize the channel structure */
	chan = devm_kzalloc(xdev->dev, sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return -ENOMEM;

	chan->dev = xdev->dev;
	chan->xdev = xdev;
	chan->has_sg = xdev->has_sg;
	chan->desc_pendingcount = 0x0;

	spin_lock_init(&chan->lock);
	INIT_LIST_HEAD(&chan->pending_list);
	INIT_LIST_HEAD(&chan->done_list);
	INIT_LIST_HEAD(&chan->active_list);

	/* Retrieve the channel properties from the device tree */
	has_dre = of_property_read_bool(node, "xlnx,include-dre");

	chan->genlock = of_property_read_bool(node, "xlnx,genlock-mode");

	err = of_property_read_u32(node, "xlnx,datawidth", &value);
	if (err) {
		dev_err(xdev->dev, "missing xlnx,datawidth property\n");
		return err;
	}
	width = value >> 3; /* Convert bits to bytes */

	/* If data width is greater than 8 bytes, DRE is not in hw */
	if (width > 8)
		has_dre = false;

	if (!has_dre)
		xdev->common.copy_align = fls(width - 1);

	if (of_device_is_compatible(node, "xlnx,axi-vdma-mm2s-channel")) {
		chan->direction = DMA_MEM_TO_DEV;
		chan->id = 0;

		chan->ctrl_offset = XILINX_VDMA_MM2S_CTRL_OFFSET;
		if (xdev->quirks & AXIVDMA_SUPPORT) {
			chan->desc_offset = XILINX_VDMA_MM2S_DESC_OFFSET;

			if (xdev->flush_on_fsync == XILINX_VDMA_FLUSH_BOTH ||
			    xdev->flush_on_fsync == XILINX_VDMA_FLUSH_MM2S)
				chan->flush_on_fsync = true;
		}
	} else if (of_device_is_compatible(node,
					    "xlnx,axi-vdma-s2mm-channel")) {
		chan->direction = DMA_DEV_TO_MEM;
		chan->id = 1;

		chan->ctrl_offset = XILINX_VDMA_S2MM_CTRL_OFFSET;
		if (xdev->quirks & AXIVDMA_SUPPORT) {
			chan->desc_offset = XILINX_VDMA_S2MM_DESC_OFFSET;

			if (xdev->flush_on_fsync == XILINX_VDMA_FLUSH_BOTH ||
			    xdev->flush_on_fsync == XILINX_VDMA_FLUSH_S2MM)
				chan->flush_on_fsync = true;
		}
	} else {
		dev_err(xdev->dev, "Invalid channel compatible node\n");
		return -EINVAL;
	}

	if (xdev->quirks & AXIVDMA_SUPPORT)
		chan->start_transfer = xilinx_vdma_start_transfer;
	else
		chan->start_transfer = xilinx_dma_start_transfer;

	/* Request the interrupt */
	chan->irq = irq_of_parse_and_map(node, 0);
	err = request_irq(chan->irq, xilinx_vdma_irq_handler, IRQF_SHARED,
			  "xilinx-vdma-controller", chan);
	if (err) {
		dev_err(xdev->dev, "unable to request IRQ %d\n", chan->irq);
		return err;
	}

	/* Initialize the tasklet */
	tasklet_init(&chan->tasklet, xilinx_vdma_do_tasklet,
			(unsigned long)chan);

	/*
	 * Initialize the DMA channel and add it to the DMA engine channels
	 * list.
	 */
	chan->common.device = &xdev->common;

	list_add_tail(&chan->common.device_node, &xdev->common.channels);
	xdev->chan[chan->id] = chan;

	/* Reset the channel */
	err = xilinx_vdma_chan_reset(chan);
	if (err < 0) {
		dev_err(xdev->dev, "Reset channel failed\n");
		return err;
	}

	return 0;
}

/**
 * of_dma_xilinx_xlate - Translation function
 * @dma_spec: Pointer to DMA specifier as found in the device tree
 * @ofdma: Pointer to DMA controller data
 *
 * Return: DMA channel pointer on success and NULL on error
 */
static struct dma_chan *of_dma_xilinx_xlate(struct of_phandle_args *dma_spec,
						struct of_dma *ofdma)
{
	struct xilinx_vdma_device *xdev = ofdma->of_dma_data;
	int chan_id = dma_spec->args[0];

	if (chan_id >= XILINX_VDMA_MAX_CHANS_PER_DEVICE)
		return NULL;

	return dma_get_slave_channel(&xdev->chan[chan_id]->common);
}

static const struct xdma_platform_data xvdma_def = {
	.quirks = AXIVDMA_SUPPORT,
};

static const struct xdma_platform_data xdma_def = {
	.quirks = AXIDMA_SUPPORT,
};

static const struct of_device_id xilinx_vdma_of_ids[] = {
	{ .compatible = "xlnx,axi-vdma-1.00.a", .data = &xvdma_def},
	{ .compatible = "xlnx,axi-dma-1.00.a", .data = &xdma_def},
	{}
};
MODULE_DEVICE_TABLE(of, xilinx_vdma_of_ids);

/**
 * xilinx_vdma_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_vdma_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct xilinx_vdma_device *xdev;
	struct device_node *child;
	struct resource *io;
	const struct of_device_id *match;
	u32 num_frames;
	int i, err;

	/* Allocate and initialize the DMA engine structure */
	xdev = devm_kzalloc(&pdev->dev, sizeof(*xdev), GFP_KERNEL);
	if (!xdev)
		return -ENOMEM;

	match = of_match_node(xilinx_vdma_of_ids, pdev->dev.of_node);
	if (match && match->data) {
		const struct xdma_platform_data *data = match->data;

		xdev->quirks = data->quirks;
	}

	xdev->dev = &pdev->dev;

	/* Request and map I/O memory */
	io = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xdev->regs = devm_ioremap_resource(&pdev->dev, io);
	if (IS_ERR(xdev->regs))
		return PTR_ERR(xdev->regs);

	/* Retrieve the DMA engine properties from the device tree */
	xdev->has_sg = of_property_read_bool(node, "xlnx,include-sg");

	if ((xdev->quirks & AXIVDMA_SUPPORT)) {

		err = of_property_read_u32(node, "xlnx,num-fstores",
					   &num_frames);
		if (err < 0) {
			dev_err(xdev->dev,
				"missing xlnx,num-fstores property\n");
			return err;
		}

		err = of_property_read_u32(node, "xlnx,flush-fsync",
						&xdev->flush_on_fsync);
		if (err < 0)
			dev_warn(xdev->dev,
				 "missing xlnx,flush-fsync property\n");
	}

	/* Initialize the DMA engine */
	xdev->common.dev = &pdev->dev;

	INIT_LIST_HEAD(&xdev->common.channels);
	dma_cap_set(DMA_SLAVE, xdev->common.cap_mask);
	dma_cap_set(DMA_PRIVATE, xdev->common.cap_mask);

	xdev->common.device_alloc_chan_resources =
				xilinx_vdma_alloc_chan_resources;
	xdev->common.device_free_chan_resources =
				xilinx_vdma_free_chan_resources;
	xdev->common.device_terminate_all = xilinx_vdma_terminate_all;
	xdev->common.device_tx_status = xilinx_vdma_tx_status;
	if (xdev->quirks & AXIVDMA_SUPPORT) {
		xdev->common.device_issue_pending = xilinx_vdma_issue_pending;
		xdev->common.device_prep_interleaved_dma =
				xilinx_vdma_dma_prep_interleaved;
	} else {
		xdev->common.device_prep_slave_sg = xilinx_dma_prep_slave_sg;
		xdev->common.device_issue_pending = xilinx_dma_issue_pending;
		xdev->common.directions = BIT(DMA_DEV_TO_MEM) |
					  BIT(DMA_MEM_TO_DEV);
		xdev->common.residue_granularity =
					  DMA_RESIDUE_GRANULARITY_SEGMENT;
	}

	platform_set_drvdata(pdev, xdev);

	/* Initialize the channels */
	for_each_child_of_node(node, child) {
		err = xilinx_vdma_chan_probe(xdev, child);
		if (err < 0)
			goto error;
	}

	if (xdev->quirks & AXIVDMA_SUPPORT) {
		for (i = 0; i < XILINX_VDMA_MAX_CHANS_PER_DEVICE; i++)
			if (xdev->chan[i])
				xdev->chan[i]->num_frms = num_frames;
	}

	/* Register the DMA engine with the core */
	dma_async_device_register(&xdev->common);

	err = of_dma_controller_register(node, of_dma_xilinx_xlate,
					 xdev);
	if (err < 0) {
		dev_err(&pdev->dev, "Unable to register DMA to DT\n");
		dma_async_device_unregister(&xdev->common);
		goto error;
	}

	dev_info(&pdev->dev, "Xilinx AXI VDMA Engine Driver Probed!!\n");

	return 0;

error:
	for (i = 0; i < XILINX_VDMA_MAX_CHANS_PER_DEVICE; i++)
		if (xdev->chan[i])
			xilinx_vdma_chan_remove(xdev->chan[i]);

	return err;
}

/**
 * xilinx_vdma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 *
 * Return: Always '0'
 */
static int xilinx_vdma_remove(struct platform_device *pdev)
{
	struct xilinx_vdma_device *xdev = platform_get_drvdata(pdev);
	int i;

	of_dma_controller_free(pdev->dev.of_node);

	dma_async_device_unregister(&xdev->common);

	for (i = 0; i < XILINX_VDMA_MAX_CHANS_PER_DEVICE; i++)
		if (xdev->chan[i])
			xilinx_vdma_chan_remove(xdev->chan[i]);

	return 0;
}

static struct platform_driver xilinx_vdma_driver = {
	.driver = {
		.name = "xilinx-vdma",
		.of_match_table = xilinx_vdma_of_ids,
	},
	.probe = xilinx_vdma_probe,
	.remove = xilinx_vdma_remove,
};

module_platform_driver(xilinx_vdma_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx VDMA driver");
MODULE_LICENSE("GPL v2");
