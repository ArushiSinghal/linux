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
#include "hns_roce_common.h"
#include "hns_roce_device.h"
#include "hns_roce_icm.h"

void hns_roce_unregister_device(struct hns_roce_dev *hr_dev)
{
	ib_unregister_device(&hr_dev->ib_dev);
}

int hns_roce_register_device(struct hns_roce_dev *hr_dev)
{
	int ret;
	struct hns_roce_ib_iboe *iboe = NULL;
	struct ib_device *ib_dev = NULL;
	struct device *dev = &hr_dev->pdev->dev;

	iboe = &hr_dev->iboe;

	ib_dev = &hr_dev->ib_dev;
	strlcpy(ib_dev->name, "hisi_%d", IB_DEVICE_NAME_MAX);

	ib_dev->owner			= THIS_MODULE;
	ib_dev->node_type		= RDMA_NODE_IB_CA;
	ib_dev->dma_device		= dev;

	ib_dev->phys_port_cnt		= hr_dev->caps.num_ports;
	ib_dev->local_dma_lkey		= hr_dev->caps.reserved_lkey;
	ib_dev->num_comp_vectors	= hr_dev->caps.num_comp_vectors;
	ib_dev->uverbs_abi_ver		= 1;

	ret = ib_register_device(ib_dev, NULL);
	if (ret) {
		dev_err(dev, "ib_register_device failed!\n");
		return ret;
	}

	return 0;
}


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

void hns_roce_profile_init(struct hns_roce_dev *hr_dev)
{
	hr_dev->hw->hw_profile(hr_dev);
}

int hns_roce_init_icm(struct hns_roce_dev *hr_dev)
{
	int ret;
	struct device *dev = &hr_dev->pdev->dev;

	ret = hns_roce_init_icm_table(hr_dev,
				      (void *)&hr_dev->mr_table.mtt_table,
				      ICM_TYPE_MTT, hr_dev->caps.mtt_entry_sz,
				      hr_dev->caps.num_mtt_segs, 0, 1, 0);
	if (ret) {
		dev_err(dev, "Failed to map MTT context memory, aborting.\n");
		return ret;
	}

	ret = hns_roce_init_icm_table(hr_dev,
				      (void *)&hr_dev->mr_table.mtpt_table,
				      ICM_TYPE_MTPT, hr_dev->caps.mtpt_entry_sz,
				      hr_dev->caps.num_mtpts, 0, 1, 1);
	if (ret) {
		dev_err(dev, "Failed to map dMPT context memory, aborting.\n");
		goto err_unmap_mtt;
	}

	ret = hns_roce_init_icm_table(hr_dev,
				      (void *)&hr_dev->qp_table.qp_table,
				      ICM_TYPE_QPC, hr_dev->caps.qpc_entry_sz,
				      hr_dev->caps.num_qps, 0, 1, 0);
	if (ret) {
		dev_err(dev, "Failed to map QP context memory, aborting.\n");
		goto err_unmap_dmpt;
	}

	ret = hns_roce_init_icm_table(hr_dev,
				      (void *)&hr_dev->qp_table.irrl_table,
				      ICM_TYPE_IRRL,
				      hr_dev->caps.irrl_entry_sz *
				      hr_dev->caps.max_qp_init_rdma,
				      hr_dev->caps.num_qps, 0, 1, 0);
	if (ret) {
		dev_err(dev, "Failed to map irrl_table memory, aborting.\n");
		goto err_unmap_qp;
	}

	ret = hns_roce_init_icm_table(hr_dev,
				      (void *)&hr_dev->cq_table.table,
				      ICM_TYPE_CQC, hr_dev->caps.cqc_entry_sz,
				      hr_dev->caps.num_cqs, 0, 1, 0);
	if (ret) {
		dev_err(dev, "Failed to map CQ context memory, aborting.\n");
		goto err_unmap_irrl;
	}

	return 0;

err_unmap_irrl:
	hns_roce_cleanup_icm_table(hr_dev,
				   (void *)&hr_dev->qp_table.irrl_table);

err_unmap_qp:
	hns_roce_cleanup_icm_table(hr_dev, (void *)&hr_dev->qp_table.qp_table);

err_unmap_dmpt:
	hns_roce_cleanup_icm_table(hr_dev,
				   (void *)&hr_dev->mr_table.mtpt_table);

err_unmap_mtt:
	hns_roce_cleanup_icm_table(hr_dev, (void *)&hr_dev->mr_table.mtt_table);

	return ret;
}

int hns_roce_engine_init(struct hns_roce_dev  *hr_dev)
{
	return hr_dev->hw->hw_init(hr_dev);
}

void hns_roce_engine_uninit(struct hns_roce_dev  *hr_dev)
{
	hr_dev->hw->hw_uninit(hr_dev);
}

/**
* hns_roce_setup_hca - setup host channel adapter
* @hr_dev: pointer to hns roce device
* Return : int
*/
int hns_roce_setup_hca(struct hns_roce_dev *hr_dev)
{
	int ret;
	struct device *dev = &hr_dev->pdev->dev;

	spin_lock_init(&hr_dev->sm_lock);
	spin_lock_init(&hr_dev->cq_db_lock);
	spin_lock_init(&hr_dev->bt_cmd_lock);

	ret = hns_roce_init_uar_table(hr_dev);
	if (ret) {
		dev_err(dev, "Failed to initialize uar table. aborting\n");
		return ret;
	}

	ret = hns_roce_uar_alloc(hr_dev, &hr_dev->priv_uar);
	if (ret) {
		dev_err(dev, "Failed to allocate priv_uar.\n");
		goto err_uar_table_free;
	}

	ret = hns_roce_init_pd_table(hr_dev);
	if (ret) {
		dev_err(dev, "Failed to init protected domain table.\n");
		goto err_uar_alloc_free;
	}

	ret = hns_roce_init_mr_table(hr_dev);
	if (ret) {
		dev_err(dev, "Failed to init memory region table.\n");
		goto err_pd_table_free;
	}

	ret = hns_roce_init_cq_table(hr_dev);
	if (ret) {
		dev_err(dev, "Failed to init completion queue table.\n");
		goto err_mr_table_free;
	}

	ret = hns_roce_init_qp_table(hr_dev);
	if (ret) {
		dev_err(dev, "Failed to init queue pair table.\n");
		goto err_cq_table_free;
	}

	return 0;

err_cq_table_free:
	hns_roce_cleanup_cq_table(hr_dev);

err_mr_table_free:
	hns_roce_cleanup_mr_table(hr_dev);

err_pd_table_free:
	hns_roce_cleanup_pd_table(hr_dev);

err_uar_alloc_free:
	hns_roce_uar_free(hr_dev, &hr_dev->priv_uar);

err_uar_table_free:
	hns_roce_cleanup_uar_table(hr_dev);
	return ret;
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

	hns_roce_profile_init(hr_dev);

	ret = hns_roce_cmd_init(hr_dev);
	if (ret) {
		dev_err(dev, "cmd init failed!\n");
		goto error_failed_cmd_init;
	}

	ret = hns_roce_init_eq_table(hr_dev);
	if (ret) {
		dev_err(dev, "eq init failed!\n");
		goto error_failed_eq_table;
	}

	if (hr_dev->cmd_mod) {
		ret = hns_roce_cmd_use_events(hr_dev);
		if (ret) {
			dev_err(dev, "Switch to event-driven cmd failed!\n");
			goto error_failed_use_event;
		}
	}

	ret = hns_roce_init_icm(hr_dev);
	if (ret) {
		dev_err(dev, "init icm fail!\n");
		goto error_failed_init_icm;
	}

	ret = hns_roce_setup_hca(hr_dev);
	if (ret) {
		dev_err(dev, "setup hca fail!\n");
		goto error_failed_setup_hca;
	}

	ret = hns_roce_engine_init(hr_dev);
	if (ret) {
		dev_err(dev, "hw_init failed!\n");
		goto error_failed_engine_init;
	}

	ret = hns_roce_register_device(hr_dev);
	if (ret) {
		dev_err(dev, "register_device failed!\n");
		goto error_failed_register_device;
	}

	return 0;

error_failed_register_device:
	hns_roce_engine_uninit(hr_dev);

error_failed_engine_init:
	hns_roce_cleanup_bitmap(hr_dev);

error_failed_setup_hca:
	hns_roce_cleanup_icm(hr_dev);

error_failed_init_icm:
	if (hr_dev->cmd_mod)
		hns_roce_cmd_use_polling(hr_dev);

error_failed_use_event:
	hns_roce_cleanup_eq_table(hr_dev);

error_failed_eq_table:
	hns_roce_cmd_cleanup(hr_dev);

error_failed_cmd_init:
	ret = hns_roce_engine_reset(hr_dev, 0);
	if (ret)
		dev_err(&hr_dev->pdev->dev, "roce_engine reset fail\n");

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

	hns_roce_unregister_device(hr_dev);
	hns_roce_engine_uninit(hr_dev);
	hns_roce_cleanup_bitmap(hr_dev);
	hns_roce_cleanup_icm(hr_dev);

	if (hr_dev->cmd_mod)
		hns_roce_cmd_use_polling(hr_dev);

	hns_roce_cleanup_eq_table(hr_dev);
	hns_roce_cmd_cleanup(hr_dev);

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
