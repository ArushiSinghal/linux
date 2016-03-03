/**
 * intel-mux-drcfg.c - Driver for Intel USB mux via register
 *
 * Copyright (C) 2016 Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/usb/mux.h>
#include <linux/platform_device.h>

#define INTEL_MUX_CFG0		0x00
#define INTEL_MUX_CFG1		0x04
#define CFG0_SW_IDPIN		BIT(20)
#define CFG0_SW_IDPIN_EN	BIT(21)
#define CFG0_SW_VBUS_VALID	BIT(24)
#define CFG1_SW_MODE		BIT(29)
#define CFG1_POLL_TIMEOUT	1000

struct intel_usb_mux {
	struct usb_mux_dev umdev;
	void __iomem *regs;
	u32 cfg0_ctx;
};

static inline int intel_mux_drcfg_switch(struct usb_mux_dev *umdev, bool host)
{
	struct intel_usb_mux *mux;
	unsigned long timeout;
	u32 data;

	mux = container_of(umdev, struct intel_usb_mux, umdev);

	/* Check and set mux to SW controlled mode */
	data = readl(mux->regs + INTEL_MUX_CFG0);
	if (!(data & CFG0_SW_IDPIN_EN)) {
		data |= CFG0_SW_IDPIN_EN;
		writel(data, mux->regs + INTEL_MUX_CFG0);
	}

	/*
	 * Configure CFG0 to switch the mux and VBUS_VALID bit is
	 * required for device mode.
	 */
	data = readl(mux->regs + INTEL_MUX_CFG0);
	if (host)
		data &= ~(CFG0_SW_IDPIN | CFG0_SW_VBUS_VALID);
	else
		data |= (CFG0_SW_IDPIN | CFG0_SW_VBUS_VALID);
	writel(data, mux->regs + INTEL_MUX_CFG0);

	/*
	 * Polling CFG1 for safety, most case it takes about 600ms
	 * to finish mode switching, set TIMEOUT long enough.
	 */
	timeout = jiffies + msecs_to_jiffies(CFG1_POLL_TIMEOUT);

	/* Polling on CFG1 register to confirm mode switch. */
	while (!time_after(jiffies, timeout)) {
		data = readl(mux->regs + INTEL_MUX_CFG1);
		if (!(host ^ (data & CFG1_SW_MODE)))
			return 0;
		/* interval for polling is set to about 5ms */
		usleep_range(5000, 5100);
	}

	return -ETIMEDOUT;
}

static int intel_mux_drcfg_cable_set(struct usb_mux_dev *umdev)
{
	dev_dbg(umdev->dev, "drcfg mux switch to HOST\n");

	return intel_mux_drcfg_switch(umdev, true);
}

static int intel_mux_drcfg_cable_unset(struct usb_mux_dev *umdev)
{
	dev_dbg(umdev->dev, "drcfg mux switch to DEVICE\n");

	return intel_mux_drcfg_switch(umdev, false);
}

static int intel_mux_drcfg_probe(struct platform_device *pdev)
{
	struct intel_usb_mux *mux;
	struct usb_mux_dev *umdev;
	struct device *dev = &pdev->dev;
	u64 start, size;
	int ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	ret = device_property_read_u64(dev, "reg-start", &start);
	ret |= device_property_read_u64(dev, "reg-size", &size);
	if (ret)
		return -ENODEV;

	mux->regs = devm_ioremap_nocache(dev, start, size);
	if (!mux->regs)
		return -ENOMEM;

	mux->cfg0_ctx = readl(mux->regs + INTEL_MUX_CFG0);

	umdev = &mux->umdev;
	umdev->dev = dev;
	umdev->cable_name = "USB-HOST";
	umdev->cable_set_cb = intel_mux_drcfg_cable_set;
	umdev->cable_unset_cb = intel_mux_drcfg_cable_unset;

	ret = usb_mux_register(umdev);
	if (ret)
		writel(mux->cfg0_ctx, mux->regs + INTEL_MUX_CFG0);

	return ret;
}

static int intel_mux_drcfg_remove(struct platform_device *pdev)
{
	struct usb_mux_dev *umdev = usb_mux_get_dev(&pdev->dev);
	struct intel_usb_mux *mux = container_of(umdev,
				struct intel_usb_mux, umdev);

	writel(mux->cfg0_ctx, mux->regs + INTEL_MUX_CFG0);

	return usb_mux_unregister(&pdev->dev);
}

#ifdef CONFIG_PM_SLEEP
/*
 * In case a micro A cable was plugged in while device was sleeping,
 * we missed the interrupt. We need to poll usb id state when waking
 * the driver to detect the missed event.
 * We use 'complete' callback to give time to all extcon listeners to
 * resume before we send new events.
 */
static const struct dev_pm_ops intel_mux_drcfg_pm_ops = {
	.complete = usb_mux_complete,
};
#endif

static const struct platform_device_id intel_mux_drcfg_platform_ids[] = {
	{ .name = "intel-mux-drcfg", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, intel_mux_drcfg_platform_ids);

static struct platform_driver intel_mux_drcfg_driver = {
	.probe		= intel_mux_drcfg_probe,
	.remove		= intel_mux_drcfg_remove,
	.driver		= {
		.name	= "intel-mux-drcfg",
#ifdef CONFIG_PM_SLEEP
		.pm	= &intel_mux_drcfg_pm_ops,
#endif
	},
	.id_table = intel_mux_drcfg_platform_ids,
};

module_platform_driver(intel_mux_drcfg_driver);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_AUTHOR("Lu Baolu <baolu.lu@linux.intel.com>");
MODULE_DESCRIPTION("Intel USB drcfg mux driver");
MODULE_LICENSE("GPL v2");
