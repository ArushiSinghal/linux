/*
 * Qualcomm Technologies HIDMA DMA engine interface
 *
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Copyright (C) Freescale Semicondutor, Inc. 2007, 2008.
 * Copyright (C) Semihalf 2009
 * Copyright (C) Ilya Yanok, Emcraft Systems 2010
 * Copyright (C) Alexander Popov, Promcontroller 2014
 *
 * Written by Piotr Ziecik <kosmo@semihalf.com>. Hardware description
 * (defines, structures and comments) was taken from MPC5121 DMA driver
 * written by Hongjun Chen <hong-jun.chen@freescale.com>.
 *
 * Approved as OSADL project by a majority of OSADL members and funded
 * by OSADL membership fees in 2009;  for details see www.osadl.org.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in the
 * file called COPYING.
 */

/* Linux Foundation elects GPLv2 license only. */

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/of_dma.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/acpi.h>
#include <linux/irq.h>
#include <linux/atomic.h>
#include <linux/pm_runtime.h>

#include "../dmaengine.h"
#include "hidma.h"

/*
 * Default idle time is 2 seconds. This parameter can
 * be overridden by changing the following
 * /sys/bus/platform/devices/QCOM8061:<xy>/power/autosuspend_delay_ms
 * during kernel boot.
 */
#define AUTOSUSPEND_TIMEOUT		2000
#define ERR_INFO_SW			0xFF
#define ERR_CODE_UNEXPECTED_TERMINATE	0x0

static inline
struct hidma_dev *to_hidma_dev(struct dma_device *dmadev)
{
	return container_of(dmadev, struct hidma_dev, ddev);
}

static inline
struct hidma_dev *to_hidma_dev_from_lldev(struct hidma_lldev **_lldevp)
{
	return container_of(_lldevp, struct hidma_dev, lldev);
}

static inline
struct hidma_chan *to_hidma_chan(struct dma_chan *dmach)
{
	return container_of(dmach, struct hidma_chan, chan);
}

static inline struct hidma_desc *
to_hidma_desc(struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct hidma_desc, desc);
}

static void hidma_free(struct hidma_dev *dmadev)
{
	dev_dbg(dmadev->ddev.dev, "free dmadev\n");
	INIT_LIST_HEAD(&dmadev->ddev.channels);
}

static unsigned int nr_desc_prm;
module_param(nr_desc_prm, uint, 0644);
MODULE_PARM_DESC(nr_desc_prm,
		 "number of descriptors (default: 0)");

#define MAX_HIDMA_CHANNELS	64
static int event_channel_idx[MAX_HIDMA_CHANNELS] = {
	[0 ... (MAX_HIDMA_CHANNELS - 1)] = -1};
static unsigned int num_event_channel_idx;
module_param_array_named(event_channel_idx, event_channel_idx, int,
			&num_event_channel_idx, 0644);
MODULE_PARM_DESC(event_channel_idx,
		"event channel index array for the notifications");
static atomic_t channel_ref_count;

/* process completed descriptors */
static void hidma_process_completed(struct hidma_dev *mdma)
{
	dma_cookie_t last_cookie = 0;
	struct hidma_chan *mchan;
	struct hidma_desc *mdesc;
	struct dma_async_tx_descriptor *desc;
	unsigned long irqflags;
	struct list_head list;
	struct dma_chan *dmach = NULL;

	list_for_each_entry(dmach, &mdma->ddev.channels,
			device_node) {
		mchan = to_hidma_chan(dmach);
		INIT_LIST_HEAD(&list);

		/* Get all completed descriptors */
		spin_lock_irqsave(&mchan->lock, irqflags);
		list_splice_tail_init(&mchan->completed, &list);
		spin_unlock_irqrestore(&mchan->lock, irqflags);

		/* Execute callbacks and run dependencies */
		list_for_each_entry(mdesc, &list, node) {
			desc = &mdesc->desc;

			spin_lock_irqsave(&mchan->lock, irqflags);
			dma_cookie_complete(desc);
			spin_unlock_irqrestore(&mchan->lock, irqflags);

			if (desc->callback &&
				(hidma_ll_status(mdma->lldev, mdesc->tre_ch)
				== DMA_COMPLETE))
				desc->callback(desc->callback_param);

			last_cookie = desc->cookie;
			dma_run_dependencies(desc);
		}

		/* Free descriptors */
		spin_lock_irqsave(&mchan->lock, irqflags);
		list_splice_tail_init(&list, &mchan->free);
		spin_unlock_irqrestore(&mchan->lock, irqflags);
	}
}

/*
 * Called once for each submitted descriptor.
 * PM is locked once for each descriptor that is currently
 * in execution.
 */
static void hidma_callback(void *data)
{
	struct hidma_desc *mdesc = data;
	struct hidma_chan *mchan = to_hidma_chan(mdesc->desc.chan);
	unsigned long irqflags;
	struct dma_device *ddev = mchan->chan.device;
	struct hidma_dev *dmadev = to_hidma_dev(ddev);
	bool queued = false;

	dev_dbg(dmadev->ddev.dev, "callback: data:0x%p\n", data);

	spin_lock_irqsave(&mchan->lock, irqflags);

	if (mdesc->node.next) {
		/* Delete from the active list, add to completed list */
		list_move_tail(&mdesc->node, &mchan->completed);
		queued = true;
	}
	spin_unlock_irqrestore(&mchan->lock, irqflags);

	hidma_process_completed(dmadev);

	if (queued) {
		pm_runtime_mark_last_busy(dmadev->ddev.dev);
		pm_runtime_put_autosuspend(dmadev->ddev.dev);
	}
}

static int hidma_chan_init(struct hidma_dev *dmadev, u32 dma_sig)
{
	struct hidma_chan *mchan;
	struct dma_device *ddev;

	mchan = devm_kzalloc(dmadev->ddev.dev, sizeof(*mchan), GFP_KERNEL);
	if (!mchan)
		return -ENOMEM;

	ddev = &dmadev->ddev;
	mchan->dma_sig = dma_sig;
	mchan->dmadev = dmadev;
	mchan->chan.device = ddev;
	dma_cookie_init(&mchan->chan);

	INIT_LIST_HEAD(&mchan->free);
	INIT_LIST_HEAD(&mchan->prepared);
	INIT_LIST_HEAD(&mchan->active);
	INIT_LIST_HEAD(&mchan->completed);

	spin_lock_init(&mchan->lock);
	list_add_tail(&mchan->chan.device_node, &ddev->channels);
	dmadev->ddev.chancnt++;
	return 0;
}

static void hidma_issue_pending(struct dma_chan *dmach)
{
	struct hidma_chan *mchan = to_hidma_chan(dmach);
	struct hidma_dev *dmadev = mchan->dmadev;

	/* PM will be released in hidma_callback function. */
	pm_runtime_get_sync(dmadev->ddev.dev);
	hidma_ll_start(dmadev->lldev);
}

static enum dma_status hidma_tx_status(struct dma_chan *dmach,
					dma_cookie_t cookie,
					struct dma_tx_state *txstate)
{
	enum dma_status ret;
	struct hidma_chan *mchan = to_hidma_chan(dmach);

	if (mchan->paused)
		ret = DMA_PAUSED;
	else
		ret = dma_cookie_status(dmach, cookie, txstate);

	return ret;
}

/*
 * Submit descriptor to hardware.
 * Lock the PM for each descriptor we are sending.
 */
static dma_cookie_t hidma_tx_submit(struct dma_async_tx_descriptor *txd)
{
	struct hidma_chan *mchan = to_hidma_chan(txd->chan);
	struct hidma_dev *dmadev = mchan->dmadev;
	struct hidma_desc *mdesc;
	unsigned long irqflags;
	dma_cookie_t cookie;

	if (!hidma_ll_isenabled(dmadev->lldev))
		return -ENODEV;

	mdesc = container_of(txd, struct hidma_desc, desc);
	spin_lock_irqsave(&mchan->lock, irqflags);

	/* Move descriptor to active */
	list_move_tail(&mdesc->node, &mchan->active);

	/* Update cookie */
	cookie = dma_cookie_assign(txd);

	hidma_ll_queue_request(dmadev->lldev, mdesc->tre_ch);
	spin_unlock_irqrestore(&mchan->lock, irqflags);

	return cookie;
}

static int hidma_alloc_chan_resources(struct dma_chan *dmach)
{
	struct hidma_chan *mchan = to_hidma_chan(dmach);
	struct hidma_dev *dmadev = mchan->dmadev;
	int rc = 0;
	struct hidma_desc *mdesc, *tmp;
	unsigned long irqflags;
	LIST_HEAD(descs);
	u32 i;

	if (mchan->allocated)
		return 0;

	/* Alloc descriptors for this channel */
	for (i = 0; i < dmadev->nr_descriptors; i++) {
		mdesc = kzalloc(sizeof(struct hidma_desc), GFP_KERNEL);
		if (!mdesc) {
			rc = -ENOMEM;
			break;
		}
		dma_async_tx_descriptor_init(&mdesc->desc, dmach);
		mdesc->desc.flags = DMA_CTRL_ACK;
		mdesc->desc.tx_submit = hidma_tx_submit;

		rc = hidma_ll_request(dmadev->lldev,
				mchan->dma_sig, "DMA engine", hidma_callback,
				mdesc, &mdesc->tre_ch);
		if (rc) {
			dev_err(dmach->device->dev,
				"channel alloc failed at %u\n", i);
			kfree(mdesc);
			break;
		}
		list_add_tail(&mdesc->node, &descs);
	}

	if (rc) {
		/* return the allocated descriptors */
		list_for_each_entry_safe(mdesc, tmp, &descs, node) {
			hidma_ll_free(dmadev->lldev, mdesc->tre_ch);
			kfree(mdesc);
		}
		return rc;
	}

	spin_lock_irqsave(&mchan->lock, irqflags);
	list_splice_tail_init(&descs, &mchan->free);
	mchan->allocated = true;
	spin_unlock_irqrestore(&mchan->lock, irqflags);
	dev_dbg(dmadev->ddev.dev,
		"allocated channel for %u\n", mchan->dma_sig);
	return 1;
}

static void hidma_free_chan_resources(struct dma_chan *dmach)
{
	struct hidma_chan *mchan = to_hidma_chan(dmach);
	struct hidma_dev *mdma = mchan->dmadev;
	struct hidma_desc *mdesc, *tmp;
	unsigned long irqflags;
	LIST_HEAD(descs);

	if (!list_empty(&mchan->prepared) ||
		!list_empty(&mchan->active) ||
		!list_empty(&mchan->completed)) {
		/*
		 * We have unfinished requests waiting.
		 * Terminate the request from the hardware.
		 */
		hidma_cleanup_pending_tre(mdma->lldev, ERR_INFO_SW,
				ERR_CODE_UNEXPECTED_TERMINATE);

		/* Give enough time for completions to be called. */
		msleep(100);
	}

	spin_lock_irqsave(&mchan->lock, irqflags);
	/* Channel must be idle */
	WARN_ON(!list_empty(&mchan->prepared));
	WARN_ON(!list_empty(&mchan->active));
	WARN_ON(!list_empty(&mchan->completed));

	/* Move data */
	list_splice_tail_init(&mchan->free, &descs);

	/* Free descriptors */
	list_for_each_entry_safe(mdesc, tmp, &descs, node) {
		hidma_ll_free(mdma->lldev, mdesc->tre_ch);
		list_del(&mdesc->node);
		kfree(mdesc);
	}

	mchan->allocated = 0;
	spin_unlock_irqrestore(&mchan->lock, irqflags);
	dev_dbg(mdma->ddev.dev, "freed channel for %u\n", mchan->dma_sig);
}


static struct dma_async_tx_descriptor *
hidma_prep_dma_memcpy(struct dma_chan *dmach, dma_addr_t dma_dest,
			dma_addr_t dma_src, size_t len, unsigned long flags)
{
	struct hidma_chan *mchan = to_hidma_chan(dmach);
	struct hidma_desc *mdesc = NULL;
	struct hidma_dev *mdma = mchan->dmadev;
	unsigned long irqflags;

	dev_dbg(mdma->ddev.dev,
		"memcpy: chan:%p dest:%pad src:%pad len:%zu\n", mchan,
		&dma_dest, &dma_src, len);

	/* Get free descriptor */
	spin_lock_irqsave(&mchan->lock, irqflags);
	if (!list_empty(&mchan->free)) {
		mdesc = list_first_entry(&mchan->free, struct hidma_desc,
					node);
		list_del(&mdesc->node);
	}
	spin_unlock_irqrestore(&mchan->lock, irqflags);

	if (!mdesc)
		return NULL;

	hidma_ll_set_transfer_params(mdma->lldev, mdesc->tre_ch,
			dma_src, dma_dest, len, flags);

	/* Place descriptor in prepared list */
	spin_lock_irqsave(&mchan->lock, irqflags);
	list_add_tail(&mdesc->node, &mchan->prepared);
	spin_unlock_irqrestore(&mchan->lock, irqflags);

	return &mdesc->desc;
}

static int hidma_terminate_all(struct dma_chan *chan)
{
	struct hidma_dev *dmadev;
	LIST_HEAD(head);
	unsigned long irqflags;
	LIST_HEAD(list);
	struct hidma_desc *tmp, *mdesc = NULL;
	int rc;
	struct hidma_chan *mchan;

	mchan = to_hidma_chan(chan);
	dmadev = to_hidma_dev(mchan->chan.device);
	dev_dbg(dmadev->ddev.dev, "terminateall: chan:0x%p\n", mchan);

	pm_runtime_get_sync(dmadev->ddev.dev);
	/* give completed requests a chance to finish */
	hidma_process_completed(dmadev);

	spin_lock_irqsave(&mchan->lock, irqflags);
	list_splice_init(&mchan->active, &list);
	list_splice_init(&mchan->prepared, &list);
	list_splice_init(&mchan->completed, &list);
	spin_unlock_irqrestore(&mchan->lock, irqflags);

	/* this suspends the existing transfer */
	rc = hidma_ll_pause(dmadev->lldev);
	if (rc) {
		dev_err(dmadev->ddev.dev, "channel did not pause\n");
		goto out;
	}

	/* return all user requests */
	list_for_each_entry_safe(mdesc, tmp, &list, node) {
		struct dma_async_tx_descriptor	*txd = &mdesc->desc;
		dma_async_tx_callback callback = mdesc->desc.callback;
		void *param = mdesc->desc.callback_param;
		enum dma_status status;

		dma_descriptor_unmap(txd);

		status = hidma_ll_status(dmadev->lldev, mdesc->tre_ch);
		/*
		 * The API requires that no submissions are done from a
		 * callback, so we don't need to drop the lock here
		 */
		if (callback && (status == DMA_COMPLETE))
			callback(param);

		dma_run_dependencies(txd);

		/* move myself to free_list */
		list_move(&mdesc->node, &mchan->free);
	}

	/* reinitialize the hardware */
	rc = hidma_ll_setup(dmadev->lldev);

out:
	pm_runtime_mark_last_busy(dmadev->ddev.dev);
	pm_runtime_put_autosuspend(dmadev->ddev.dev);
	return rc;
}

static int hidma_pause(struct dma_chan *chan)
{
	struct hidma_chan *mchan;
	struct hidma_dev *dmadev;

	mchan = to_hidma_chan(chan);
	dmadev = to_hidma_dev(mchan->chan.device);
	dev_dbg(dmadev->ddev.dev, "pause: chan:0x%p\n", mchan);

	if (!mchan->paused) {
		pm_runtime_get_sync(dmadev->ddev.dev);
		if (hidma_ll_pause(dmadev->lldev))
			dev_warn(dmadev->ddev.dev, "channel did not stop\n");
		mchan->paused = true;
		pm_runtime_mark_last_busy(dmadev->ddev.dev);
		pm_runtime_put_autosuspend(dmadev->ddev.dev);
	}
	return 0;
}

static int hidma_resume(struct dma_chan *chan)
{
	struct hidma_chan *mchan;
	struct hidma_dev *dmadev;
	int rc = 0;

	mchan = to_hidma_chan(chan);
	dmadev = to_hidma_dev(mchan->chan.device);
	dev_dbg(dmadev->ddev.dev, "resume: chan:0x%p\n", mchan);

	if (mchan->paused) {
		pm_runtime_get_sync(dmadev->ddev.dev);
		rc = hidma_ll_resume(dmadev->lldev);
		if (!rc)
			mchan->paused = false;
		else
			dev_err(dmadev->ddev.dev,
					"failed to resume the channel");
		pm_runtime_mark_last_busy(dmadev->ddev.dev);
		pm_runtime_put_autosuspend(dmadev->ddev.dev);
	}
	return rc;
}

static irqreturn_t hidma_chirq_handler(int chirq, void *arg)
{
	struct hidma_lldev **lldev_ptr = arg;
	irqreturn_t ret;
	struct hidma_dev *dmadev = to_hidma_dev_from_lldev(lldev_ptr);

	/*
	 * All interrupts are request driven.
	 * HW doesn't send an interrupt by itself.
	 */
	pm_runtime_get_sync(dmadev->ddev.dev);
	ret = hidma_ll_inthandler(chirq, *lldev_ptr);
	pm_runtime_mark_last_busy(dmadev->ddev.dev);
	pm_runtime_put_autosuspend(dmadev->ddev.dev);
	return ret;
}

static int hidma_probe(struct platform_device *pdev)
{
	struct hidma_dev *dmadev;
	int rc = 0;
	struct resource *trca_resource;
	struct resource *evca_resource;
	int chirq;
	int current_channel_index = atomic_read(&channel_ref_count);
	void *evca;
	void *trca;

	pm_runtime_set_autosuspend_delay(&pdev->dev, AUTOSUSPEND_TIMEOUT);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	trca_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!trca_resource) {
		rc = -ENODEV;
		goto bailout;
	}

	trca = devm_ioremap_resource(&pdev->dev, trca_resource);
	if (IS_ERR(trca)) {
		rc = -ENOMEM;
		goto bailout;
	}

	evca_resource = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!evca_resource) {
		rc = -ENODEV;
		goto bailout;
	}

	evca = devm_ioremap_resource(&pdev->dev, evca_resource);
	if (IS_ERR(evca)) {
		rc = -ENOMEM;
		goto bailout;
	}

	/*
	 * This driver only handles the channel IRQs.
	 * Common IRQ is handled by the management driver.
	 */
	chirq = platform_get_irq(pdev, 0);
	if (chirq < 0) {
		rc = -ENODEV;
		goto bailout;
	}

	dmadev = devm_kzalloc(&pdev->dev, sizeof(*dmadev), GFP_KERNEL);
	if (!dmadev) {
		rc = -ENOMEM;
		goto bailout;
	}

	INIT_LIST_HEAD(&dmadev->ddev.channels);
	spin_lock_init(&dmadev->lock);
	dmadev->ddev.dev = &pdev->dev;
	pm_runtime_get_sync(dmadev->ddev.dev);

	dma_cap_set(DMA_MEMCPY, dmadev->ddev.cap_mask);
	if (WARN_ON(!pdev->dev.dma_mask)) {
		rc = -ENXIO;
		goto dmafree;
	}

	dmadev->dev_evca = evca;
	dmadev->evca_resource = evca_resource;
	dmadev->dev_trca = trca;
	dmadev->trca_resource = trca_resource;
	dmadev->ddev.device_prep_dma_memcpy = hidma_prep_dma_memcpy;
	dmadev->ddev.device_alloc_chan_resources =
		hidma_alloc_chan_resources;
	dmadev->ddev.device_free_chan_resources = hidma_free_chan_resources;
	dmadev->ddev.device_tx_status = hidma_tx_status;
	dmadev->ddev.device_issue_pending = hidma_issue_pending;
	dmadev->ddev.device_pause = hidma_pause;
	dmadev->ddev.device_resume = hidma_resume;
	dmadev->ddev.device_terminate_all = hidma_terminate_all;
	dmadev->ddev.copy_align = 8;

	device_property_read_u32(&pdev->dev, "desc-count",
				&dmadev->nr_descriptors);

	if (!dmadev->nr_descriptors && nr_desc_prm)
		dmadev->nr_descriptors = nr_desc_prm;

	if (!dmadev->nr_descriptors)
		goto dmafree;

	if (current_channel_index > MAX_HIDMA_CHANNELS)
		goto dmafree;

	dmadev->evridx = -1;
	device_property_read_u32(&pdev->dev, "event-channel", &dmadev->evridx);

	/* kernel command line override for the guest machine */
	if (event_channel_idx[current_channel_index] != -1)
		dmadev->evridx = event_channel_idx[current_channel_index];

	if (dmadev->evridx == -1)
		goto dmafree;

	/* Set DMA mask to 64 bits. */
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (rc) {
		dev_warn(&pdev->dev, "unable to set coherent mask to 64");
		rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (rc)
			goto dmafree;
	}

	dmadev->lldev = hidma_ll_init(dmadev->ddev.dev,
				dmadev->nr_descriptors, dmadev->dev_trca,
				dmadev->dev_evca, dmadev->evridx);
	if (!dmadev->lldev) {
		rc = -EPROBE_DEFER;
		goto dmafree;
	}

	rc = devm_request_irq(&pdev->dev, chirq, hidma_chirq_handler, 0,
			      "qcom-hidma", &dmadev->lldev);
	if (rc)
		goto uninit;

	INIT_LIST_HEAD(&dmadev->ddev.channels);
	rc = hidma_chan_init(dmadev, 0);
	if (rc)
		goto uninit;

	rc = dma_selftest_memcpy(&dmadev->ddev);
	if (rc)
		goto uninit;

	rc = dma_async_device_register(&dmadev->ddev);
	if (rc)
		goto uninit;

	hidma_debug_init(dmadev);
	dev_info(&pdev->dev, "HI-DMA engine driver registration complete\n");
	platform_set_drvdata(pdev, dmadev);
	pm_runtime_mark_last_busy(dmadev->ddev.dev);
	pm_runtime_put_autosuspend(dmadev->ddev.dev);
	atomic_inc(&channel_ref_count);
	return 0;

uninit:
	hidma_debug_uninit(dmadev);
	hidma_ll_uninit(dmadev->lldev);
dmafree:
	if (dmadev)
		hidma_free(dmadev);
bailout:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_sync_suspend(&pdev->dev);
	return rc;
}

static int hidma_remove(struct platform_device *pdev)
{
	struct hidma_dev *dmadev = platform_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "removing\n");
	pm_runtime_get_sync(dmadev->ddev.dev);

	dma_async_device_unregister(&dmadev->ddev);
	hidma_debug_uninit(dmadev);
	hidma_ll_uninit(dmadev->lldev);
	hidma_free(dmadev);

	dev_info(&pdev->dev, "HI-DMA engine removed\n");
	pm_runtime_put_sync_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

#if IS_ENABLED(CONFIG_ACPI)
static const struct acpi_device_id hidma_acpi_ids[] = {
	{"QCOM8061"},
	{},
};
#endif

static const struct of_device_id hidma_match[] = {
	{ .compatible = "qcom,hidma-1.0", },
	{},
};
MODULE_DEVICE_TABLE(of, hidma_match);

static struct platform_driver hidma_driver = {
	.probe = hidma_probe,
	.remove = hidma_remove,
	.driver = {
		.name = "hidma",
		.of_match_table = hidma_match,
		.acpi_match_table = ACPI_PTR(hidma_acpi_ids),
	},
};
module_platform_driver(hidma_driver);
MODULE_LICENSE("GPL v2");
