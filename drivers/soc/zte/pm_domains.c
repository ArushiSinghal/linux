/*
 * Copyright (C) 2015 ZTE Ltd.
 *
 * Author: Baoyou Xie <baoyou.xie@linaro.org>
 * License terms: GNU General Public License (GPL) version 2
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include "pm_domains.h"

#define PCU_DM_CLKEN(zpd)	((zpd)->reg_offset[REG_CLKEN])
#define PCU_DM_ISOEN(zpd)	((zpd)->reg_offset[REG_ISOEN])
#define PCU_DM_RSTEN(zpd)	((zpd)->reg_offset[REG_RSTEN])
#define PCU_DM_PWREN(zpd)	((zpd)->reg_offset[REG_PWREN])
#define PCU_DM_PWRDN(zpd)	((zpd)->reg_offset[REG_PWRDN])
#define PCU_DM_ACK_SYNC(zpd)	((zpd)->reg_offset[REG_ACK_SYNC])

static void __iomem *pcubase;

int zx_normal_power_on(struct generic_pm_domain *domain)
{
	struct zx_pm_domain *zpd = (struct zx_pm_domain *)domain;
	unsigned long loop = 1000;
	u32 val;

	if (zpd->polarity == PWREN) {
		val = readl_relaxed(pcubase + PCU_DM_PWREN(zpd));
		val |= BIT(zpd->bit);
		writel_relaxed(val, pcubase + PCU_DM_PWREN(zpd));
	} else {
		val = readl_relaxed(pcubase + PCU_DM_PWRDN(zpd));
		val &= ~BIT(zpd->bit);
		writel_relaxed(val, pcubase + PCU_DM_PWRDN(zpd));
	}

	do {
		udelay(1);
		val = readl_relaxed(pcubase + PCU_DM_ACK_SYNC(zpd))
				   & BIT(zpd->bit);
	} while (--loop && !val);

	if (!loop) {
		pr_err("Error: %s %s fail\n", __func__, domain->name);
		return -EIO;
	}

	val = readl_relaxed(pcubase + PCU_DM_RSTEN(zpd));
	val &= ~BIT(zpd->bit);
	writel_relaxed(val | BIT(zpd->bit), pcubase + PCU_DM_RSTEN(zpd));
	udelay(5);

	val = readl_relaxed(pcubase + PCU_DM_ISOEN(zpd));
	val &= ~BIT(zpd->bit);
	writel_relaxed(val, pcubase + PCU_DM_ISOEN(zpd));
	udelay(5);

	val = readl_relaxed(pcubase + PCU_DM_CLKEN(zpd));
	val &= ~BIT(zpd->bit);
	writel_relaxed(val | BIT(zpd->bit), pcubase + PCU_DM_CLKEN(zpd));
	udelay(5);

	pr_info("normal poweron %s\n", domain->name);

	return 0;
}

int zx_normal_power_off(struct generic_pm_domain *domain)
{
	struct zx_pm_domain *zpd = (struct zx_pm_domain *)domain;
	unsigned long loop = 1000;
	u32 val;

	val = readl_relaxed(pcubase + PCU_DM_CLKEN(zpd));
	val &= ~BIT(zpd->bit);
	writel_relaxed(val, pcubase + PCU_DM_CLKEN(zpd));
	udelay(5);

	val = readl_relaxed(pcubase + PCU_DM_ISOEN(zpd));
	val &= ~BIT(zpd->bit);
	writel_relaxed(val | BIT(zpd->bit), pcubase + PCU_DM_ISOEN(zpd));
	udelay(5);

	val = readl_relaxed(pcubase + PCU_DM_RSTEN(zpd));
	val &= ~BIT(zpd->bit);
	writel_relaxed(val, pcubase + PCU_DM_RSTEN(zpd));
	udelay(5);

	if (zpd->polarity == PWREN) {
		val = readl_relaxed(pcubase + PCU_DM_PWREN(zpd));
		val &= ~BIT(zpd->bit);
		writel_relaxed(val, pcubase + PCU_DM_PWREN(zpd));
	} else {
		val = readl_relaxed(pcubase + PCU_DM_PWRDN(zpd));
		val |= BIT(zpd->bit);
		writel_relaxed(val, pcubase + PCU_DM_PWRDN(zpd));
	}

	do {
		udelay(1);
		val = readl_relaxed(pcubase + PCU_DM_ACK_SYNC(zpd))
				   & BIT(zpd->bit);
	} while (--loop && val);

	if (!loop) {
		pr_err("Error: %s %s fail\n", __func__, domain->name);
		return -EIO;
	}

	pr_info("normal poweroff %s\n", domain->name);

	return 0;
}

int
zx_pd_probe(struct platform_device *pdev,
	   struct generic_pm_domain **zx_pm_domains,
	   int domain_num)
{
	struct genpd_onecell_data *genpd_data;
	struct resource *res;
	int i;

	genpd_data = devm_kzalloc(&pdev->dev, sizeof(*genpd_data), GFP_KERNEL);
	if (!genpd_data)
		return -ENOMEM;

	genpd_data->domains = zx_pm_domains;
	genpd_data->num_domains = domain_num;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no memory resource defined\n");
		return -ENODEV;
	}

	pcubase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pcubase)) {
		dev_err(&pdev->dev, "ioremap fail.\n");
		return -EIO;
	}

	for (i = 0; i < domain_num; ++i)
		pm_genpd_init(zx_pm_domains[i], NULL, false);

	of_genpd_add_provider_onecell(pdev->dev.of_node, genpd_data);
	dev_info(&pdev->dev, "powerdomain init ok\n");
	return 0;
}
