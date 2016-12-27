/*
 * Aquantia Corporation Network Driver
 * Copyright (C) 2014-2016 Aquantia Corporation. All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

/*
 * File aq_vec.c: Definition of common structures for vector of Rx and Tx rings.
 * Declaration of functions for Rx and Tx rings.
 */

#ifndef AQ_VEC_H
#define AQ_VEC_H

#include "aq_common.h"

#include <linux/irqreturn.h>

struct aq_hw_s;
struct aq_hw_ops;

int aq_vec_poll(struct napi_struct *napi, int budget);
irqreturn_t aq_vec_isr(int irq, void *private);
irqreturn_t aq_vec_isr_legacy(int irq, void *private);
struct aq_vec_s *aq_vec_alloc(struct aq_nic_s *aq_nic, unsigned int idx,
			      struct aq_nic_cfg_s *aq_nic_cfg);
int aq_vec_init(struct aq_vec_s *self, struct aq_hw_ops *aq_hw_ops,
		struct aq_hw_s *aq_hw);
int aq_vec_deinit(struct aq_vec_s *self);
void aq_vec_free(struct aq_vec_s *self);
int aq_vec_start(struct aq_vec_s *self);
int aq_vec_stop(struct aq_vec_s *self);
cpumask_t *aq_vec_get_affinity_mask(struct aq_vec_s *self);
int aq_vec_get_sw_stats(struct aq_vec_s *self, u64 *data,
			unsigned int *p_count);

#endif /* AQ_VEC_H */
