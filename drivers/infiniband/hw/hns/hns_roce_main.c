/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * Authors: Wei Hu <xavier.huwei@huawei.com>
 * Authors: Znlong <zhaonenglong@hisilicon.com>
 * Authors: oulijun <oulijun@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/ethtool.h>
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <net/ipv6.h>
#include <net/addrconf.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>
#include "hns_roce_device.h"

int hns_roce_get_cfg(struct hns_roce_dev *hr_dev)
{
	int i;
	u8 phy_port;
	int port_cnt = 0;
	struct device *dev = &hr_dev->pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *net_node;
	struct net_device *netdev = NULL;
	struct platform_device *pdev = NULL;
	struct resource *res;

	if (of_device_is_compatible(np, "hisilicon,hns-roce-v1")) {
		hr_dev->hw = &hns_roce_hw_v1;
	} else {
		dev_err(dev, "device no compatible!\n");
		return -EINVAL;
	}

	res = platform_get_resource(hr_dev->pdev, IORESOURCE_MEM, 0);
	hr_dev->reg_base = devm_ioremap_resource(dev, res);
	if (!hr_dev->reg_base) {
		dev_err(dev, "devm_ioremap_resource failed!\n");
		return -ENOMEM;
	}

	for (i = 0; i < HNS_ROCE_MAX_PORTS; i++) {
		net_node = of_parse_phandle(np, "eth-handle", i);
		if (net_node) {
			pdev = of_find_device_by_node(net_node);
			netdev = platform_get_drvdata(pdev);
			phy_port = (u8)i;
			if (netdev) {
				hr_dev->iboe.netdevs[port_cnt] = netdev;
				hr_dev->iboe.phy_port[port_cnt] = phy_port;
			} else {
				return -ENODEV;
			}
			port_cnt++;
		}
	}

	hr_dev->caps.num_ports = port_cnt;

	/* Cmd issue mode: 0 is poll, 1 is event */
	hr_dev->cmd_mod = 1;
	hr_dev->loop_idc = 0;

	for (i = 0; i < HNS_ROCE_MAX_IRQ_NUM; i++) {
		hr_dev->irq[i] = platform_get_irq(hr_dev->pdev, i);
		if (hr_dev->irq[i] <= 0) {
			dev_err(dev, "Get No.%d irq resource failed!\n", i);
			return -EINVAL;
		}
	}

	return 0;
}

int hns_roce_engine_reset(struct hns_roce_dev *hr_dev, u32 val)
{
	return hr_dev->hw->reset(hr_dev, val);
}
/**
* hns_roce_probe - RoCE driver entrance
* @pdev: pointer to platform device
* Return : int
*
*/
static int hns_roce_probe(struct platform_device *pdev)
{
	int ret;
	struct hns_roce_dev *hr_dev;
	struct device *dev = &pdev->dev;

	hr_dev = (struct hns_roce_dev *)ib_alloc_device(sizeof(*hr_dev));
	if (!hr_dev) {
		dev_err(dev, "Device struct alloc failed, aborting.\n");
		return -ENOMEM;
	}

	memset((u8 *)hr_dev + sizeof(struct ib_device), 0,
		sizeof(struct hns_roce_dev) - sizeof(struct ib_device));

	hr_dev->pdev = pdev;
	platform_set_drvdata(pdev, hr_dev);

	if (!dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64ULL)))
		dev_info(dev, "set mask to 64bit\n");
	else if (!dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32ULL)))
		dev_info(dev, "set mask to 32bit\n");
	else {
		dev_err(dev, "No usable DMA addressing mode\n");
		ret = -EIO;
		goto error_failed_get_cfg;
	}
	ret = hns_roce_get_cfg(hr_dev);
	if (ret) {
		dev_err(dev, "Get Configuration failed!\n");
		goto error_failed_get_cfg;
	}

	ret = hns_roce_engine_reset(hr_dev, 1);
	if (ret) {
		dev_err(dev, "Reset roce engine failed!\n");
		goto error_failed_get_cfg;
	}

error_failed_get_cfg:
	ib_dealloc_device(&hr_dev->ib_dev);
	return ret;
}

/**
* hns_roce_remove - remove roce device
* @pdev: pointer to platform device
*/
static int hns_roce_remove(struct platform_device *pdev)
{
	struct hns_roce_dev *hr_dev = platform_get_drvdata(pdev);
	int ret = 0;

	ret = hns_roce_engine_reset(hr_dev, 0);
	if (ret)
		return ret;
	ib_dealloc_device(&hr_dev->ib_dev);

	return ret;
}

static const struct of_device_id hns_roce_of_match[] = {
	{ .compatible = "hisilicon,hns-roce-v1",},
	{},
};
MODULE_DEVICE_TABLE(of, hns_roce_of_match);

static struct platform_driver hns_roce_driver = {
	.probe = hns_roce_probe,
	.remove = hns_roce_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = hns_roce_of_match,
	},
};

module_platform_driver(hns_roce_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Wei Hu <xavier.huwei@huawei.com>");
MODULE_AUTHOR("Znlong <zhaonenglong@hisilicon.com>");
MODULE_AUTHOR("oulijun <oulijun@huawei.com>");
MODULE_DESCRIPTION("HISILICON RoCE driver");
MODULE_ALIAS("platform:" DRV_NAME);
