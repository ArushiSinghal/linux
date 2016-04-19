/*
 * Reserved IOVA Management
 *
 * Copyright (c) 2015 Linaro Ltd.
 *              www.linaro.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/iommu.h>
#include <linux/iova.h>

struct reserved_iova_domain {
	struct iova_domain *iovad;
	int prot; /* iommu protection attributes to be obeyed */
};

struct iommu_reserved_binding {
	struct kref		kref;
	struct rb_node		node;
	struct iommu_domain	*domain;
	phys_addr_t		addr;
	dma_addr_t		iova;
	size_t			size;
};

/* Reserved binding RB-tree manipulation */

/* @d->reserved_lock must be held */
static struct iommu_reserved_binding *find_reserved_binding(
				    struct iommu_domain *d,
				    phys_addr_t start, size_t size)
{
	struct rb_node *node = d->reserved_binding_list.rb_node;

	while (node) {
		struct iommu_reserved_binding *binding =
			rb_entry(node, struct iommu_reserved_binding, node);

		if (start + size <= binding->addr)
			node = node->rb_left;
		else if (start >= binding->addr + binding->size)
			node = node->rb_right;
		else
			return binding;
	}

	return NULL;
}

/* @d->reserved_lock must be held */
static void link_reserved_binding(struct iommu_domain *d,
				  struct iommu_reserved_binding *new)
{
	struct rb_node **link = &d->reserved_binding_list.rb_node;
	struct rb_node *parent = NULL;
	struct iommu_reserved_binding *binding;

	while (*link) {
		parent = *link;
		binding = rb_entry(parent, struct iommu_reserved_binding,
				   node);

		if (new->addr + new->size <= binding->addr)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, &d->reserved_binding_list);
}

/* @d->reserved_lock must be held */
static void unlink_reserved_binding(struct iommu_domain *d,
				    struct iommu_reserved_binding *old)
{
	rb_erase(&old->node, &d->reserved_binding_list);
}

int iommu_alloc_reserved_iova_domain(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size, int prot,
				     unsigned long order)
{
	unsigned long granule, mask, flags;
	struct reserved_iova_domain *rid;
	int ret = 0;

	granule = 1UL << order;
	mask = granule - 1;
	if (iova & mask || (!size) || (size & mask))
		return -EINVAL;

	rid = kzalloc(sizeof(struct reserved_iova_domain), GFP_KERNEL);
	if (!rid)
		return -ENOMEM;

	rid->iovad = kzalloc(sizeof(struct iova_domain), GFP_KERNEL);
	if (!rid->iovad) {
		kfree(rid);
		return -ENOMEM;
	}

	iova_cache_get();

	init_iova_domain(rid->iovad, granule,
			 iova >> order, (iova + size - 1) >> order);

	spin_lock_irqsave(&domain->reserved_lock, flags);

	if (domain->reserved_iova_cookie) {
		ret = -EEXIST;
		goto unlock;
	}

	domain->reserved_iova_cookie = rid;

unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	if (ret) {
		put_iova_domain(rid->iovad);
		kfree(rid->iovad);
		kfree(rid);
		iova_cache_put();
	}
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_alloc_reserved_iova_domain);

void iommu_free_reserved_iova_domain(struct iommu_domain *domain)
{
	struct reserved_iova_domain *rid;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&domain->reserved_lock, flags);

	rid = (struct reserved_iova_domain *)domain->reserved_iova_cookie;
	if (!rid) {
		ret = -EINVAL;
		goto unlock;
	}

	domain->reserved_iova_cookie = NULL;
unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	if (!ret) {
		put_iova_domain(rid->iovad);
		kfree(rid->iovad);
		kfree(rid);
		iova_cache_put();
	}
}
EXPORT_SYMBOL_GPL(iommu_free_reserved_iova_domain);
