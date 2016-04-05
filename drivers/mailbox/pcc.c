/*
 *	Copyright (C) 2014 Linaro Ltd.
 *	Author:	Ashwin Chaugule <ashwin.chaugule@linaro.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  PCC (Platform Communication Channel) is defined in the ACPI 5.0+
 *  specification. It is a mailbox like mechanism to allow clients
 *  such as CPPC (Collaborative Processor Performance Control), RAS
 *  (Reliability, Availability and Serviceability) and MPST (Memory
 *  Node Power State Table) to talk to the platform (e.g. BMC) through
 *  shared memory regions as defined in the PCC table entries. The PCC
 *  specification supports a Doorbell mechanism for the PCC clients
 *  to notify the platform about new data. This Doorbell information
 *  is also specified in each PCC table entry.
 *
 *  Typical high level flow of operation is:
 *
 *  PCC Reads:
 *  * Client tries to acquire a channel lock.
 *  * After it is acquired it writes READ cmd in communication region cmd
 *		address.
 *  * Client issues mbox_send_message() which rings the PCC doorbell
 *		for its PCC channel.
 *  * If command completes, then client has control over channel and
 *		it can proceed with its reads.
 *  * Client releases lock.
 *
 *  PCC Writes:
 *  * Client tries to acquire channel lock.
 *  * Client writes to its communication region after it acquires a
 *		channel lock.
 *  * Client writes WRITE cmd in communication region cmd address.
 *  * Client issues mbox_send_message() which rings the PCC doorbell
 *		for its PCC channel.
 *  * If command completes, then writes have succeded and it can release
 *		the channel lock.
 *
 *  There is a Nominal latency defined for each channel which indicates
 *  how long to wait until a command completes. If command is not complete
 *  the client needs to retry or assume failure.
 *
 *	For more details about PCC, please see the ACPI specification from
 *  http://www.uefi.org/ACPIv5.1 Section 14.
 *
 *  This file implements PCC as a Mailbox controller and allows for PCC
 *  clients to be implemented as its Mailbox Client Channels.
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox_client.h>
#include <linux/io-64-nonatomic-lo-hi.h>

#include "mailbox.h"

#define MAX_PCC_SUBSPACES	256
#define MBOX_IRQ_NAME		"pcc-mbox"

/**
 * PCC mailbox channel information
 *
 * @chan:	Pointer to mailbox communication channel
 * @pcc_doorbell_vaddr: PCC doorbell register address
 * @pcc_doorbell_ack_vaddr: PCC doorbell ack register address
 * @irq:	Interrupt number of the channel
 */
struct pcc_mbox_chan {
	struct mbox_chan	*chan;
	void __iomem		*pcc_doorbell_vaddr;
	void __iomem		*pcc_doorbell_ack_vaddr;
	int			irq;
};

/**
 * PCC mailbox controller data
 *
 * @mb_ctrl:	Representation of the communication channel controller
 * @mbox_chan:	Array of PCC mailbox channels of the controller
 * @chans:	Array of mailbox communication channels
 */
struct pcc_mbox {
	struct mbox_controller	mbox_ctrl;
	struct pcc_mbox_chan	*mbox_chans;
	struct mbox_chan	*chans;
};

static struct pcc_mbox pcc_mbox_ctx = {};

/**
 * get_pcc_channel - Given a PCC subspace idx, get
 *	the respective pcc mbox_channel.
 * @id: PCC subspace index.
 *
 * Return: ERR_PTR(errno) if error, else pointer
 *	to pcc mbox channel.
 */
static struct pcc_mbox_chan *get_pcc_channel(int id)
{
	if (id < 0 || id > pcc_mbox_ctx.mbox_ctrl.num_chans)
		return ERR_PTR(-ENOENT);

	return &pcc_mbox_ctx.mbox_chans[id];
}

/*
 * PCC can be used with perf critical drivers such as CPPC
 * So it makes sense to locally cache the virtual address and
 * use it to read/write to PCC registers such as doorbell register
 *
 * The below read_register and write_registers are used to read and
 * write from perf critical registers such as PCC doorbell register
 */
static int read_register(void __iomem *vaddr, u64 *val, unsigned int bit_width)
{
	int ret_val = 0;

	switch (bit_width) {
	case 8:
		*val = readb(vaddr);
		break;
	case 16:
		*val = readw(vaddr);
		break;
	case 32:
		*val = readl(vaddr);
		break;
	case 64:
		*val = readq(vaddr);
		break;
	default:
		pr_debug("Error: Cannot read register of %u bit width",
			bit_width);
		ret_val = -EFAULT;
		break;
	}
	return ret_val;
}

static int write_register(void __iomem *vaddr, u64 val, unsigned int bit_width)
{
	int ret_val = 0;

	switch (bit_width) {
	case 8:
		writeb(val, vaddr);
		break;
	case 16:
		writew(val, vaddr);
		break;
	case 32:
		writel(val, vaddr);
		break;
	case 64:
		writeq(val, vaddr);
		break;
	default:
		pr_debug("Error: Cannot write register of %u bit width",
			bit_width);
		ret_val = -EFAULT;
		break;
	}
	return ret_val;
}

/**
 * pcc_map_interrupt - Map a PCC subspace GSI to a linux IRQ number
 * @interrupt: GSI number.
 * @flags: interrupt flags
 *
 * Returns: a valid linux IRQ number on success
 *		0 or -EINVAL on failure
 */
static int pcc_map_interrupt(u32 interrupt, u32 flags)
{
	int trigger, polarity;

	if (!interrupt)
		return 0;

	trigger = (flags & ACPI_PCCT_INTERRUPT_MODE) ? ACPI_EDGE_SENSITIVE
			: ACPI_LEVEL_SENSITIVE;

	polarity = (flags & ACPI_PCCT_INTERRUPT_POLARITY) ? ACPI_ACTIVE_LOW
			: ACPI_ACTIVE_HIGH;

	return acpi_register_gsi(NULL, interrupt, trigger, polarity);
}

/**
 * pcc_mbox_irq - PCC mailbox interrupt handler
 */
static irqreturn_t pcc_mbox_irq(int irq, void *id)
{
	struct acpi_generic_address *doorbell_ack;
	struct acpi_pcct_hw_reduced *pcct_ss;
	struct pcc_mbox_chan *pcc_chan = id;
	u64 doorbell_ack_preserve;
	struct mbox_chan *chan;
	u64 doorbell_ack_write;
	u64 doorbell_ack_val;
	int ret = 0;

	chan = pcc_chan->chan;
	pcct_ss = chan->con_priv;

	/* Clear interrupt status */
	if (pcct_ss->header.type == ACPI_PCCT_TYPE_HW_REDUCED_SUBSPACE_TYPE2) {
		doorbell_ack = &pcct_ss->doorbell_ack_register;
		doorbell_ack_preserve = pcct_ss->ack_preserve_mask;
		doorbell_ack_write = pcct_ss->ack_write_mask;

		ret = read_register(pcc_chan->pcc_doorbell_ack_vaddr,
				    &doorbell_ack_val,
				    doorbell_ack->bit_width);
		if (ret)
			return ret;

		ret = write_register(pcc_chan->pcc_doorbell_ack_vaddr,
				     (doorbell_ack_val & doorbell_ack_preserve)
					| doorbell_ack_write,
				     doorbell_ack->bit_width);
		if (ret)
			return ret;
	}

	mbox_chan_received_data(chan, NULL);

	return IRQ_HANDLED;
}

/**
 * pcc_mbox_request_channel - PCC clients call this function to
 *		request a pointer to their PCC subspace, from which they
 *		can get the details of communicating with the remote.
 * @cl: Pointer to Mailbox client, so we know where to bind the
 *		Channel.
 * @subspace_id: The PCC Subspace index as parsed in the PCC client
 *		ACPI package. This is used to lookup the array of PCC
 *		subspaces as parsed by the PCC Mailbox controller.
 *
 * Return: Pointer to the Mailbox Channel if successful or
 *		ERR_PTR.
 */
struct mbox_chan *pcc_mbox_request_channel(struct mbox_client *cl,
		int subspace_id)
{
	struct device *dev = pcc_mbox_ctx.mbox_ctrl.dev;
	struct pcc_mbox_chan *pcc_chan;
	struct mbox_chan *chan;
	unsigned long flags;

	/*
	 * Each PCC Subspace is a Mailbox Channel.
	 * The PCC Clients get their PCC Subspace ID
	 * from their own tables and pass it here.
	 * This returns a pointer to the PCC subspace
	 * for the Client to operate on.
	 */
	pcc_chan = get_pcc_channel(subspace_id);
	if (IS_ERR(pcc_chan)) {
		dev_err(dev, "PCC Channel not found for idx: %d\n", subspace_id);
		return ERR_PTR(-EBUSY);
	}

	chan = pcc_chan->chan;
	if (IS_ERR(chan) || chan->cl) {
		dev_err(dev, "Channel not found for idx: %d\n", subspace_id);
		return ERR_PTR(-EBUSY);
	}

	spin_lock_irqsave(&chan->lock, flags);
	chan->msg_free = 0;
	chan->msg_count = 0;
	chan->active_req = NULL;
	chan->cl = cl;
	init_completion(&chan->tx_complete);

	if (chan->txdone_method == TXDONE_BY_POLL && cl->knows_txdone)
		chan->txdone_method |= TXDONE_BY_ACK;

	if (chan->mbox->txdone_irq) {
		int rc;

		rc = devm_request_irq(dev, pcc_chan->irq, pcc_mbox_irq, 0,
				      MBOX_IRQ_NAME, pcc_chan);
		if (unlikely(rc)) {
			dev_err(dev, "failed to register PCC interrupt %d\n",
				pcc_chan->irq);
			chan = ERR_PTR(rc);
		}
	}

	spin_unlock_irqrestore(&chan->lock, flags);

	return chan;
}
EXPORT_SYMBOL_GPL(pcc_mbox_request_channel);

/**
 * pcc_mbox_free_channel - Clients call this to free their Channel.
 *
 * @chan: Pointer to the mailbox channel as returned by
 *		pcc_mbox_request_channel()
 */
void pcc_mbox_free_channel(struct mbox_chan *chan)
{
	unsigned long flags;

	if (!chan || !chan->cl)
		return;

	spin_lock_irqsave(&chan->lock, flags);
	chan->cl = NULL;
	chan->active_req = NULL;
	if (chan->txdone_method == (TXDONE_BY_POLL | TXDONE_BY_ACK))
		chan->txdone_method = TXDONE_BY_POLL;

	if (chan->mbox->txdone_irq) {
		struct acpi_pcct_hw_reduced *pcct_ss = chan->con_priv;
		int irq = 0;

		if (acpi_gsi_to_irq(pcct_ss->doorbell_interrupt, &irq) == 0)
			devm_free_irq(chan->mbox->dev, irq, chan);
	}

	spin_unlock_irqrestore(&chan->lock, flags);
}
EXPORT_SYMBOL_GPL(pcc_mbox_free_channel);


/**
 * pcc_send_data - Called from Mailbox Controller code. Used
 *		here only to ring the channel doorbell. The PCC client
 *		specific read/write is done in the client driver in
 *		order to maintain atomicity over PCC channel once
 *		OS has control over it. See above for flow of operations.
 * @chan: Pointer to Mailbox channel over which to send data.
 * @data: Client specific data written over channel. Used here
 *		only for debug after PCC transaction completes.
 *
 * Return: Err if something failed else 0 for success.
 */
static int pcc_send_data(struct mbox_chan *chan, void *data)
{
	struct acpi_pcct_hw_reduced *pcct_ss = chan->con_priv;
	struct acpi_generic_address *doorbell;
	struct pcc_mbox_chan *pcc_chan;
	u64 doorbell_preserve;
	u64 doorbell_val;
	u64 doorbell_write;
	u32 id = chan - pcc_mbox_ctx.chans;
	int ret = 0;

	if (id >= pcc_mbox_ctx.mbox_ctrl.num_chans) {
		pr_debug("pcc_send_data: Invalid mbox_chan passed\n");
		return -ENOENT;
	}

	pcc_chan = get_pcc_channel(id);
	if (IS_ERR(pcc_chan)) {
		pr_debug("PCC Channel not found for idx: %d\n", id);
		return -EBUSY;
	}

	doorbell = &pcct_ss->doorbell_register;
	doorbell_preserve = pcct_ss->preserve_mask;
	doorbell_write = pcct_ss->write_mask;

	/* Sync notification from OS to Platform. */
	if (pcc_chan->pcc_doorbell_vaddr) {
		ret = read_register(pcc_chan->pcc_doorbell_vaddr, &doorbell_val,
			doorbell->bit_width);
		if (ret)
			return ret;
		ret = write_register(pcc_chan->pcc_doorbell_vaddr,
			(doorbell_val & doorbell_preserve) | doorbell_write,
			doorbell->bit_width);
	} else {
		ret = acpi_read(&doorbell_val, doorbell);
		if (ret)
			return ret;
		ret = acpi_write((doorbell_val & doorbell_preserve) | doorbell_write,
			doorbell);
	}
	return ret;
}

static const struct mbox_chan_ops pcc_chan_ops = {
	.send_data = pcc_send_data,
};

/**
 * parse_pcc_subspace - Parse the PCC table and verify PCC subspace
 *		entries. There should be one entry per PCC client.
 * @header: Pointer to the ACPI subtable header under the PCCT.
 * @end: End of subtable entry.
 *
 * Return: 0 for Success, else errno.
 *
 * This gets called for each entry in the PCC table.
 */
static int parse_pcc_subspace(struct acpi_subtable_header *header,
		const unsigned long end)
{
	struct acpi_pcct_hw_reduced *pcct_ss;

	if (pcc_mbox_ctx.mbox_ctrl.num_chans <= MAX_PCC_SUBSPACES) {
		pcct_ss = (struct acpi_pcct_hw_reduced *) header;

		if ((pcct_ss->header.type !=
				ACPI_PCCT_TYPE_HW_REDUCED_SUBSPACE)
		    && (pcct_ss->header.type !=
				ACPI_PCCT_TYPE_HW_REDUCED_SUBSPACE_TYPE2)) {
			pr_err("Incorrect PCC Subspace type detected\n");
			return -EINVAL;
		}
	}

	return 0;
}

/**
 * pcc_parse_subspace_irq - Parse the PCC IRQ and PCC ACK register
 *		There should be one entry per PCC client.
 * @mbox_chans: Pointer to the PCC mailbox channel data
 * @pcct_ss: Pointer to the ACPI subtable header under the PCCT.
 *
 * Return: 0 for Success, else errno.
 *
 * This gets called for each entry in the PCC table.
 */
static int pcc_parse_subspace_irq(struct pcc_mbox_chan *mbox_chans,
		struct acpi_pcct_hw_reduced *pcct_ss)
{
	mbox_chans->irq = pcc_map_interrupt(pcct_ss->doorbell_interrupt,
					    (u32)pcct_ss->flags);
	if (mbox_chans->irq <= 0) {
		pr_err("PCC GSI %d not registered\n",
		       pcct_ss->doorbell_interrupt);
		return -EINVAL;
	}

	if (pcct_ss->header.type
		== ACPI_PCCT_TYPE_HW_REDUCED_SUBSPACE_TYPE2) {
		mbox_chans->pcc_doorbell_ack_vaddr = acpi_os_ioremap(
				pcct_ss->doorbell_ack_register.address,
				pcct_ss->doorbell_ack_register.bit_width / 8);
		if (!mbox_chans->pcc_doorbell_ack_vaddr) {
			pr_err("Failed to ioremap PCC ACK register\n");
			return -ENOMEM;
		}
	}

	return 0;
}

/**
 * acpi_pcc_probe - Parse the ACPI tree for the PCCT.
 *
 * Return: 0 for Success, else errno.
 */
static int __init acpi_pcc_probe(void)
{
	acpi_size pcct_tbl_header_size;
	struct acpi_table_header *pcct_tbl;
	struct acpi_subtable_header *pcct_entry;
	struct acpi_table_pcct *acpi_pcct_tbl;
	int count, i, rc;
	acpi_status status = AE_OK;

	/* Search for PCCT */
	status = acpi_get_table_with_size(ACPI_SIG_PCCT, 0,
			&pcct_tbl,
			&pcct_tbl_header_size);

	if (ACPI_FAILURE(status) || !pcct_tbl) {
		pr_warn("PCCT header not found.\n");
		return -ENODEV;
	}

	count = acpi_table_parse_entries(ACPI_SIG_PCCT,
			sizeof(struct acpi_table_pcct),
			ACPI_PCCT_TYPE_HW_REDUCED_SUBSPACE,
			parse_pcc_subspace, MAX_PCC_SUBSPACES);

	if (count <= 0) {
		count = acpi_table_parse_entries(ACPI_SIG_PCCT,
				sizeof(struct acpi_table_pcct),
				ACPI_PCCT_TYPE_HW_REDUCED_SUBSPACE_TYPE2,
				parse_pcc_subspace, MAX_PCC_SUBSPACES);
	}

	if (count <= 0) {
		pr_err("Error parsing PCC subspaces from PCCT\n");
		return -EINVAL;
	}

	pcc_mbox_ctx.chans = kzalloc(sizeof(struct mbox_chan) *
				     count, GFP_KERNEL);

	if (!pcc_mbox_ctx.chans) {
		pr_err("Could not allocate space for PCC mbox channels\n");
		return -ENOMEM;
	}

	pcc_mbox_ctx.mbox_chans = kzalloc(sizeof(struct pcc_mbox_chan) *
					  count, GFP_KERNEL);
	if (!pcc_mbox_ctx.mbox_chans) {
		pr_err("Could not allocate space for PCC mbox channel data\n");
		return -ENOMEM;
	}

	/* Point to the first PCC subspace entry */
	pcct_entry = (struct acpi_subtable_header *) (
		(unsigned long) pcct_tbl + sizeof(struct acpi_table_pcct));

	acpi_pcct_tbl = (struct acpi_table_pcct *) pcct_tbl;
	if (acpi_pcct_tbl->flags & ACPI_PCCT_DOORBELL)
		pcc_mbox_ctx.mbox_ctrl.txdone_irq = true;

	for (i = 0; i < count; i++) {
		struct acpi_generic_address *db_reg;
		struct acpi_pcct_hw_reduced *pcct_ss;

		pcc_mbox_ctx.chans[i].con_priv = pcct_entry;
		pcc_mbox_ctx.chans[i].mbox = &pcc_mbox_ctx.mbox_ctrl;

		pcct_ss = (struct acpi_pcct_hw_reduced *) pcct_entry;

		pcc_mbox_ctx.mbox_chans[i].chan = &pcc_mbox_ctx.chans[i];
		if (pcc_mbox_ctx.mbox_ctrl.txdone_irq) {
			rc = pcc_parse_subspace_irq(&pcc_mbox_ctx.mbox_chans[i],
						    pcct_ss);
			if (rc < 0)
				return rc;
		}

		/* If doorbell is in system memory cache the virt address */
		pcct_ss = (struct acpi_pcct_hw_reduced *)pcct_entry;
		db_reg = &pcct_ss->doorbell_register;
		if (db_reg->space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY) {
			pcc_mbox_ctx.mbox_chans[i].pcc_doorbell_vaddr =
					acpi_os_ioremap(db_reg->address,
							db_reg->bit_width/8);
		}

		pcct_entry = (struct acpi_subtable_header *)
			((unsigned long) pcct_entry + pcct_entry->length);
	}

	pcc_mbox_ctx.mbox_ctrl.num_chans = count;

	pr_info("Detected %d PCC Subspaces\n",
		pcc_mbox_ctx.mbox_ctrl.num_chans);

	return 0;
}

/**
 * pcc_mbox_probe - Called when we find a match for the
 *	PCCT platform device. This is purely used to represent
 *	the PCCT as a virtual device for registering with the
 *	generic Mailbox framework.
 *
 * @pdev: Pointer to platform device returned when a match
 *	is found.
 *
 *	Return: 0 for Success, else errno.
 */
static int pcc_mbox_probe(struct platform_device *pdev)
{
	int ret = 0;

	pcc_mbox_ctx.mbox_ctrl.chans = pcc_mbox_ctx.chans;
	pcc_mbox_ctx.mbox_ctrl.ops = &pcc_chan_ops;
	pcc_mbox_ctx.mbox_ctrl.dev = &pdev->dev;

	pr_info("Registering PCC driver as Mailbox controller\n");
	ret = mbox_controller_register(&pcc_mbox_ctx.mbox_ctrl);

	if (ret) {
		pr_err("Err registering PCC as Mailbox controller: %d\n", ret);
		ret = -ENODEV;
	}

	return ret;
}

struct platform_driver pcc_mbox_driver = {
	.probe = pcc_mbox_probe,
	.driver = {
		.name = "PCCT",
		.owner = THIS_MODULE,
	},
};

static int __init pcc_init(void)
{
	int ret;
	struct platform_device *pcc_pdev;

	if (acpi_disabled)
		return -ENODEV;

	/* Check if PCC support is available. */
	ret = acpi_pcc_probe();

	if (ret) {
		pr_debug("ACPI PCC probe failed.\n");
		return -ENODEV;
	}

	pcc_pdev = platform_create_bundle(&pcc_mbox_driver,
			pcc_mbox_probe, NULL, 0, NULL, 0);

	if (IS_ERR(pcc_pdev)) {
		pr_debug("Err creating PCC platform bundle\n");
		return PTR_ERR(pcc_pdev);
	}

	return 0;
}

/*
 * Make PCC init postcore so that users of this mailbox
 * such as the ACPI Processor driver have it available
 * at their init.
 */
postcore_initcall(pcc_init);
