/**
 * Copyright (c) 2015-2016 Quantenna Communications, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 **/

#ifndef _QTN_FMAC_CFG80211_H_
#define _QTN_FMAC_CFG80211_H_

#include <net/cfg80211.h>

#include "core.h"

int qtnf_register_wiphy(struct qtnf_bus *bus, struct qtnf_wmac *mac);
int qtnf_del_virtual_intf(struct wiphy *wiphy, struct wireless_dev *wdev);
struct wireless_dev *qtnf_add_virtual_intf(struct wiphy *wiphy,
					   const char *name,
					   unsigned char name_assign_type,
					   enum nl80211_iftype type, u32 *flags,
					   struct vif_params *params);

#endif /* _QTN_FMAC_CFG80211_H_ */
