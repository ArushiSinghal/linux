#ifndef __6LOWPAN_I_H
#define __6LOWPAN_I_H

#include <linux/netdevice.h>

extern const struct lowpan_iphc_ctx_ops iphc_ctx_unicast_ops;
extern const struct lowpan_iphc_ctx_ops iphc_ctx_mcast_ops;

#ifdef CONFIG_6LOWPAN_DEBUGFS
int lowpan_dev_debugfs_init(struct net_device *dev);
void lowpan_dev_debugfs_uninit(struct net_device *dev);

int __init lowpan_debugfs_init(void);
void lowpan_debugfs_exit(void);
#else
static inline int lowpan_dev_debugfs_init(struct net_device *dev)
{
	return 0;
}

void lowpan_dev_debugfs_uninit(struct net_device *dev) { }

static inline int __init lowpan_debugfs_init(void)
{
	return 0;
}

static inline void lowpan_debugfs_exit(void) { }
#endif /* CONFIG_6LOWPAN_DEBUGFS */

#endif /* __6LOWPAN_I_H */
