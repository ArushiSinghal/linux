/*
 * AMD Secure Processor driver
 *
 * Copyright (C) 2017 Advanced Micro Devices, Inc.
 *
 * Authors:
 *	Brijesh Singh <brijesh.singh@amd.com>
 *	Tom Lendacky <thomas.lendacky@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SP_DEV_H__
#define __SP_DEV_H__

#include <linux/device.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/dmapool.h>
#include <linux/hw_random.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>

#define SP_MAX_NAME_LEN		32

#define CACHE_NONE			0x00
#define CACHE_WB_NO_ALLOC		0xb7

/* Structure to hold CCP device data */
struct ccp_device;
struct ccp_vdata {
	const unsigned int version;
	void (*setup)(struct ccp_device *);
	const struct ccp_actions *perform;
	const unsigned int offset;
};

/* Structure to hold SP device data */
struct sp_dev_data {
	const unsigned int bar;

	const struct ccp_vdata *ccp_vdata;
	const void *psp_vdata;
};

struct sp_device {
	struct list_head entry;

	struct device *dev;

	struct sp_dev_data *dev_data;
	unsigned int ord;
	char name[SP_MAX_NAME_LEN];

	/* Bus specific device information */
	void *dev_specific;

	/* I/O area used for device communication. */
	void __iomem *io_map;

	/* DMA caching attribute support */
	unsigned int axcache;

	bool irq_registered;

	/* get and set master device */
	struct sp_device *(*get_master_device)(void);
	void (*set_master_device)(struct sp_device *);

	unsigned int psp_irq;
	irq_handler_t psp_irq_handler;
	void *psp_irq_data;

	unsigned int ccp_irq;
	irq_handler_t ccp_irq_handler;
	void *ccp_irq_data;

	void *psp_data;
	void *ccp_data;
};

int sp_pci_init(void);
void sp_pci_exit(void);

int sp_platform_init(void);
void sp_platform_exit(void);

struct sp_device *sp_alloc_struct(struct device *dev);

int sp_init(struct sp_device *sp);
void sp_destroy(struct sp_device *sp);
struct sp_device *sp_get_master(void);

int sp_suspend(struct sp_device *sp, pm_message_t state);
int sp_resume(struct sp_device *sp);

int sp_request_psp_irq(struct sp_device *sp, irq_handler_t handler,
		       const char *name, void *data);
void sp_free_psp_irq(struct sp_device *sp, void *data);

int sp_request_ccp_irq(struct sp_device *sp, irq_handler_t handler,
		       const char *name, void *data);
void sp_free_ccp_irq(struct sp_device *sp, void *data);

void sp_set_psp_master(struct sp_device *sp);
struct sp_device *sp_get_psp_master_device(void);

#ifdef CONFIG_AMD_CCP

int ccp_dev_init(struct sp_device *sp);
void ccp_dev_destroy(struct sp_device *sp);

int ccp_dev_suspend(struct sp_device *sp, pm_message_t state);
int ccp_dev_resume(struct sp_device *sp);

#else	/* !CONFIG_AMD_CCP */

static inline int ccp_dev_init(struct sp_device *sp)
{
	return 0;
}
static inline void ccp_dev_destroy(struct sp_device *sp) { }

static inline int ccp_dev_suspend(struct sp_device *sp, pm_message_t state)
{
	return 0;
}
static inline int ccp_dev_resume(struct sp_device *sp)
{
	return 0;
}

#endif	/* CONFIG_AMD_CCP */

#endif
