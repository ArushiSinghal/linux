/*
 * drivers/net/macsec.c - MACsec device
 *
 * Copyright (c) 2015 Sabrina Dubroca <sd@queasysnail.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/module.h>
#include <crypto/aead.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <net/genetlink.h>
#include <net/sock.h>

#include <uapi/linux/if_macsec.h>

typedef u64 __bitwise sci_t;

#define MACSEC_SCI_LEN 8

/* SecTAG length = macsec_eth_header without the optional SCI */
#define MACSEC_TAG_LEN 6

struct macsec_eth_header {
	struct ethhdr eth;
	/* SecTAG */
	u8  tci_an;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	u8  short_length:6,
		  unused:2;
#elif defined(__BIG_ENDIAN_BITFIELD)
	u8        unused:2,
	    short_length:6;
#else
#error	"Please fix <asm/byteorder.h>"
#endif
	__be32 packet_number;
	u8 secure_channel_id[8]; /* optional */
} __packed;

#define MACSEC_TCI_VERSION 0x80
#define MACSEC_TCI_ES      0x40 /* end station */
#define MACSEC_TCI_SC      0x20 /* SCI present */
#define MACSEC_TCI_SCB     0x10 /* epon */
#define MACSEC_TCI_E       0x08 /* encryption */
#define MACSEC_TCI_C       0x04 /* changed text */
#define MACSEC_AN_MASK     0x03 /* association number */
#define MACSEC_TCI_CONFID  (MACSEC_TCI_E | MACSEC_TCI_C)

#define MACSEC_SHORTLEN_THR 48

#define GCM_AES_IV_LEN 12
#define DEFAULT_ICV_LEN 16

#define for_each_rxsc(secy, sc)			\
	for (sc = rcu_dereference(secy->rx_sc);	\
	     sc;				\
	     sc = rcu_dereference(sc->next))
#define for_each_rxsc_rtnl(secy, sc)			\
	for (sc = rtnl_dereference(secy->rx_sc);	\
	     sc;					\
	     sc = rtnl_dereference(sc->next))

struct gcm_iv {
	union {
		u8 secure_channel_id[8];
		sci_t sci;
	};
	__be32 pn;
};

/**
 * struct macsec_key - SA key
 * @id user-provided key identifier
 * @tfm crypto struct, key storage
 */
struct macsec_key {
	u64 id;
	struct crypto_aead *tfm;
};

/**
 * struct macsec_rx_sa - receive secure association
 * @active
 * @next_pn packet number expected for the next packet
 * @lock protects next_pn manipulations
 * @key key structure
 * @stats per-SA stats
 */
struct macsec_rx_sa {
	bool active;
	u32 next_pn;
	spinlock_t lock;
	struct macsec_key key;
	struct macsec_rx_sa_stats __percpu *stats;
	atomic_t refcnt;
	struct rcu_head rcu;
};

struct pcpu_rx_sc_stats {
	struct macsec_rx_sc_stats stats;
	struct u64_stats_sync syncp;
};

/**
 * struct macsec_rx_sc - receive secure channel
 * @sci secure channel identifier for this SC
 * @active channel is active
 * @sa array of secure associations
 * @stats per-SC stats
 */
struct macsec_rx_sc {
	struct macsec_rx_sc __rcu *next;
	sci_t sci;
	bool active;
	struct macsec_rx_sa __rcu *sa[4];
	struct pcpu_rx_sc_stats __percpu *stats;
	struct rcu_head rcu_head;
};

/**
 * struct macsec_tx_sa - transmit secure association
 * @active
 * @next_pn packet number to use for the next packet
 * @lock protects next_pn manipulations
 * @key key structure
 * @stats per-SA stats
 */
struct macsec_tx_sa {
	bool active;
	u32 next_pn;
	spinlock_t lock;
	struct macsec_key key;
	struct macsec_tx_sa_stats __percpu *stats;
	atomic_t refcnt;
	struct rcu_head rcu;
};

struct pcpu_tx_sc_stats {
	struct macsec_tx_sc_stats stats;
	struct u64_stats_sync syncp;
};

/**
 * struct macsec_tx_sc - transmit secure channel
 * @active
 * @encoding_sa association number of the SA currently in use
 * @encrypt encrypt packets on transmit, or authenticate only
 * @send_sci always include the SCI in the SecTAG
 * @end_station
 * @scb single copy broadcast flag
 * @sa array of secure associations
 * @stats stats for this TXSC
 */
struct macsec_tx_sc {
	bool active;
	u8 encoding_sa;
	bool encrypt;
	bool send_sci;
	bool end_station;
	bool scb;
	struct macsec_tx_sa __rcu *sa[4];
	struct pcpu_tx_sc_stats __percpu *stats;
};

#define MACSEC_VALIDATE_DEFAULT MACSEC_VALIDATE_STRICT

/**
 * struct macsec_secy - MACsec Security Entity
 * @netdev netdevice for this SecY
 * @n_rx_sc number of receive secure channels configured on this SecY
 * @sci secure channel identifier used for tx
 * @key_len length of keys used by the cipher suite
 * @icv_len length of ICV used by the cipher suite
 * @validate_frames validation mode
 * @operational MAC_Operational flag
 * @protect_frames enable protection for this SecY
 * @replay_protect enable packet number checks on receive
 * @replay_window size of the replay window
 * @tx_sc transmit secure channel
 * @rx_sc linked list of receive secure channels
 */
struct macsec_secy {
	struct net_device *netdev;
	unsigned int n_rx_sc;
	sci_t sci;
	u16 key_len;
	u16 icv_len;
	enum validation_type validate_frames;
	bool operational;
	bool protect_frames;
	bool replay_protect;
	u32 replay_window;
	struct macsec_tx_sc tx_sc;
	struct macsec_rx_sc __rcu *rx_sc;
};

struct pcpu_secy_stats {
	struct macsec_dev_stats stats;
	struct u64_stats_sync syncp;
};

/**
 * struct macsec_dev - private data
 * @secy SecY config
 * @real_dev pointer to underlying netdevice
 * @stats MACsec device stats
 * @secys linked list of SecY's on the underlying device
 */
struct macsec_dev {
	struct macsec_secy secy;
	struct net_device *real_dev;
	struct pcpu_secy_stats __percpu *stats;
	struct list_head secys;
};

/**
 * struct macsec_rxh_data - rx_handler private argument
 * @secys linked list of SecY's on this underlying device
 */
struct macsec_rxh_data {
	struct list_head secys;
};

static struct macsec_dev *macsec_priv(const struct net_device *dev)
{
	return (struct macsec_dev *)netdev_priv(dev);
}

static struct macsec_rxh_data *macsec_data_rcu(const struct net_device *dev)
{
	return rcu_dereference(dev->rx_handler_data);
}

static struct macsec_rxh_data *macsec_data_rtnl(const struct net_device *dev)
{
	return rtnl_dereference(dev->rx_handler_data);
}

struct macsec_cb {
	struct aead_request *req;
	union {
		struct macsec_tx_sa *tx_sa;
		struct macsec_rx_sa *rx_sa;
	};
	u8 assoc_num;
	bool valid;
	bool has_sci;
};

static struct macsec_rx_sa *macsec_rxsa_get(struct macsec_rx_sa __rcu *ptr)
{
	struct macsec_rx_sa *sa = rcu_dereference_bh(ptr);

	if (!sa || !sa->active)
		return NULL;

	if (!atomic_inc_not_zero(&sa->refcnt))
		return NULL;

	return sa;
}

static void free_rxsa(struct rcu_head *head)
{
	struct macsec_rx_sa *sa = container_of(head, struct macsec_rx_sa, rcu);

	crypto_free_aead(sa->key.tfm);
	free_percpu(sa->stats);
	kfree(sa);
}

static void macsec_rxsa_put(struct macsec_rx_sa *sa)
{
	if (atomic_dec_and_test(&sa->refcnt))
		call_rcu(&sa->rcu, free_rxsa);
}

static struct macsec_tx_sa *macsec_txsa_get(struct macsec_tx_sa __rcu *ptr)
{
	struct macsec_tx_sa *sa = rcu_dereference_bh(ptr);

	if (!sa || !sa->active)
		return NULL;

	if (!atomic_inc_not_zero(&sa->refcnt))
		return NULL;

	return sa;
}

static void free_txsa(struct rcu_head *head)
{
	struct macsec_tx_sa *sa = container_of(head, struct macsec_tx_sa, rcu);

	crypto_free_aead(sa->key.tfm);
	free_percpu(sa->stats);
	kfree(sa);
}

static void macsec_txsa_put(struct macsec_tx_sa *sa)
{
	if (atomic_dec_and_test(&sa->refcnt))
		call_rcu(&sa->rcu, free_txsa);
}

static struct macsec_cb *macsec_skb_cb(struct sk_buff *skb)
{
	BUILD_BUG_ON(sizeof(struct macsec_cb) > sizeof(skb->cb));
	return (struct macsec_cb *)skb->cb;
}

#define MACSEC_PORT_ES (htons(0x0001))
#define MACSEC_PORT_SCB (0x0000)
#define MACSEC_UNDEF_SCI ((__force sci_t)0xffffffffffffffffULL)

#define DEFAULT_SAK_LEN 16
#define DEFAULT_SEND_SCI true
#define DEFAULT_ENCRYPT false
#define DEFAULT_ENCODING_SA 0

static sci_t make_sci(u8 *addr, __be16 port)
{
	sci_t sci;

	memcpy(&sci, addr, ETH_ALEN);
	memcpy(((char *)&sci) + ETH_ALEN, &port, sizeof(port));

	return sci;
}

static sci_t macsec_frame_sci(struct macsec_eth_header *hdr, bool sci_present)
{
	sci_t sci;

	if (sci_present)
		memcpy(&sci, hdr->secure_channel_id,
		       sizeof(hdr->secure_channel_id));
	else
		sci = make_sci(hdr->eth.h_source, MACSEC_PORT_ES);

	return sci;
}

static unsigned int macsec_sectag_len(bool sci_present)
{
	return MACSEC_TAG_LEN + (sci_present ? MACSEC_SCI_LEN : 0);
}

static unsigned int macsec_hdr_len(bool sci_present)
{
	return macsec_sectag_len(sci_present) + ETH_HLEN;
}

static unsigned int macsec_extra_len(bool sci_present)
{
	return macsec_sectag_len(sci_present) + sizeof(__be16);
}

/* Fill SecTAG according to IEEE 802.1AE-2006 10.5.3 */
static void macsec_fill_sectag(struct macsec_eth_header *h,
			       const struct macsec_secy *secy, u32 pn)
{
	const struct macsec_tx_sc *tx_sc = &secy->tx_sc;

	memset(&h->tci_an, 0, macsec_sectag_len(tx_sc->send_sci));
	h->eth.h_proto = htons(ETH_P_MACSEC);

	if (tx_sc->send_sci ||
	    (secy->n_rx_sc > 1 && !tx_sc->end_station && !tx_sc->scb)) {
		h->tci_an |= MACSEC_TCI_SC;
		memcpy(&h->secure_channel_id, &secy->sci,
		       sizeof(h->secure_channel_id));
	} else {
		if (tx_sc->end_station)
			h->tci_an |= MACSEC_TCI_ES;
		if (tx_sc->scb)
			h->tci_an |= MACSEC_TCI_SCB;
	}

	h->packet_number = htonl(pn);

	/* with GCM, C/E clear for !encrypt, both set for encrypt */
	if (tx_sc->encrypt)
		h->tci_an |= MACSEC_TCI_CONFID;
	else if (secy->icv_len != DEFAULT_ICV_LEN)
		h->tci_an |= MACSEC_TCI_C;

	h->tci_an |= tx_sc->encoding_sa;
}

static void macsec_set_shortlen(struct macsec_eth_header *h, size_t data_len)
{
	if (data_len < MACSEC_SHORTLEN_THR)
		h->short_length = data_len;
}

/* validate MACsec packet according to IEEE 802.1AE-2006 9.12 */
static bool macsec_validate_skb(struct sk_buff *skb, u16 icv_len)
{
	struct macsec_eth_header *h = (struct macsec_eth_header *)skb->data;
	int len = skb->len - 2 * ETH_ALEN;

	/* a) It comprises at least 17 octets */
	if (skb->len <= 16)
		return false;

	/* b) MACsec EtherType: already checked */

	/* c) V bit is clear */
	if (h->tci_an & MACSEC_TCI_VERSION)
		return false;

	/* d) ES or SCB => !SC */
	if ((h->tci_an & MACSEC_TCI_ES || h->tci_an & MACSEC_TCI_SCB) &&
	    (h->tci_an & MACSEC_TCI_SC))
		return false;

	/* e) Bits 7 and 8 of octet 4 of the SecTAG are clear */
	if (h->unused)
		return false;

	/* rx.pn != 0 (figure 10-5) */
	if (!h->packet_number)
		return false;

	if (!(h->tci_an & MACSEC_TCI_C) && !(h->tci_an & MACSEC_TCI_SC)) {
		/* f) */
		if (h->short_length)
			return len == h->short_length + 24;
		else
			return len >= 72;
	} else if (!(h->tci_an & MACSEC_TCI_C)) {
		/* g) SCI present */
		if (h->short_length)
			return len == h->short_length + 32;
		else
			return len >= 80;
	} else if (h->tci_an & MACSEC_TCI_C && !(h->tci_an & MACSEC_TCI_SC)) {
		/* h) */
		if (h->short_length)
			return len == 8 + icv_len + h->short_length;
		else
			return len >= 8 + icv_len + 48;
	} else {
		/* i) changed text, SCI present*/
		if (h->short_length)
			return len == 16 + icv_len + h->short_length;
		else
			return len >= 16 + icv_len + 48;
	}

	return true;
}

#define MACSEC_NEEDED_HEADROOM sizeof(struct macsec_eth_header)
#define MACSEC_NEEDED_TAILROOM MACSEC_MAX_ICV_LEN

static void macsec_fill_iv(unsigned char *iv, sci_t sci, u32 pn)
{
	struct gcm_iv *gcm_iv = (struct gcm_iv *)iv;

	gcm_iv->sci = sci;
	gcm_iv->pn = htonl(pn);
}

static u32 tx_sa_update_pn(struct macsec_tx_sa *tx_sa, struct macsec_secy *secy)
{
	u32 pn;

	spin_lock_bh(&tx_sa->lock);
	pn = tx_sa->next_pn;

	tx_sa->next_pn++;
	if (tx_sa->next_pn == 0) {
		pr_notice("PN wrapped, transitionning to !oper\n");
		tx_sa->active = false;
		if (secy->protect_frames)
			secy->operational = false;
	}
	spin_unlock_bh(&tx_sa->lock);

	return pn;
}

static void macsec_encrypt_finish(struct sk_buff *skb, struct net_device *dev)
{
	struct macsec_dev *macsec = netdev_priv(dev);

	skb->dev = macsec->real_dev;
	skb_reset_mac_header(skb);
	skb->protocol = eth_hdr(skb)->h_proto;
}

static void macsec_count_tx(struct sk_buff *skb, struct macsec_tx_sc *tx_sc,
			    struct macsec_tx_sa *tx_sa)
{
	struct pcpu_tx_sc_stats *txsc_stats = this_cpu_ptr(tx_sc->stats);

	u64_stats_update_begin(&txsc_stats->syncp);
	if (tx_sc->encrypt) {
		txsc_stats->stats.OutOctetsEncrypted += skb->len;
		txsc_stats->stats.OutPktsEncrypted++;
		this_cpu_inc(tx_sa->stats->OutPktsEncrypted);
	} else {
		txsc_stats->stats.OutOctetsProtected += skb->len;
		txsc_stats->stats.OutPktsProtected++;
		this_cpu_inc(tx_sa->stats->OutPktsProtected);
	}
	u64_stats_update_end(&txsc_stats->syncp);
}

static void count_tx(struct net_device *dev, int ret, int len)
{
	if (likely(ret == NET_XMIT_SUCCESS || ret == NET_XMIT_CN)) {
		struct pcpu_sw_netstats *stats = this_cpu_ptr(dev->tstats);

		u64_stats_update_begin(&stats->syncp);
		stats->tx_packets++;
		stats->tx_bytes += len;
		u64_stats_update_end(&stats->syncp);
	} else {
		dev->stats.tx_dropped++;
	}
}

static void macsec_encrypt_done(struct crypto_async_request *base, int err)
{
	struct sk_buff *skb = base->data;
	struct net_device *dev = skb->dev;
	struct macsec_dev *macsec = macsec_priv(dev);
	struct macsec_tx_sa *sa = macsec_skb_cb(skb)->tx_sa;
	int len, ret;

	aead_request_free(macsec_skb_cb(skb)->req);

	rcu_read_lock_bh();
	macsec_encrypt_finish(skb, dev);
	macsec_count_tx(skb, &macsec->secy.tx_sc, macsec_skb_cb(skb)->tx_sa);
	len = skb->len;
	ret = dev_queue_xmit(skb);
	count_tx(dev, ret, len);
	rcu_read_unlock_bh();

	macsec_txsa_put(sa);
	dev_put(dev);
}

static struct sk_buff *macsec_encrypt(struct sk_buff *skb,
				      struct net_device *dev)
{
	int ret;
	struct scatterlist sg[MAX_SKB_FRAGS + 1];
	unsigned char iv[GCM_AES_IV_LEN];
	struct ethhdr *eth;
	struct macsec_eth_header *hh;
	size_t unprotected_len;
	struct aead_request *req;
	struct macsec_secy *secy;
	struct macsec_tx_sc *tx_sc;
	struct macsec_tx_sa *tx_sa;
	struct macsec_dev *macsec = macsec_priv(dev);
	u32 pn;

	secy = &macsec->secy;
	tx_sc = &secy->tx_sc;

	/* 10.5.1 TX SA assignment */
	tx_sa = macsec_txsa_get(tx_sc->sa[tx_sc->encoding_sa]);
	if (!tx_sa) {
		secy->operational = false;
		kfree_skb(skb);
		return ERR_PTR(-EINVAL);
	}

	if (unlikely(skb_headroom(skb) < MACSEC_NEEDED_HEADROOM ||
		     skb_tailroom(skb) < MACSEC_NEEDED_TAILROOM)) {
		struct sk_buff *nskb = skb_copy_expand(skb,
						       MACSEC_NEEDED_HEADROOM,
						       MACSEC_NEEDED_TAILROOM,
						       GFP_ATOMIC);
		if (likely(nskb)) {
			consume_skb(skb);
			skb = nskb;
		} else {
			macsec_txsa_put(tx_sa);
			kfree_skb(skb);
			return ERR_PTR(-ENOMEM);
		}
	} else {
		skb = skb_unshare(skb, GFP_ATOMIC);
		if (!skb) {
			macsec_txsa_put(tx_sa);
			return ERR_PTR(-ENOMEM);
		}
	}

	unprotected_len = skb->len;
	eth = eth_hdr(skb);
	hh = (struct macsec_eth_header *)skb_push(skb, macsec_extra_len(tx_sc->send_sci));
	memmove(hh, eth, 2 * ETH_ALEN);

	pn = tx_sa_update_pn(tx_sa, secy);
	if (pn == 0) {
		macsec_txsa_put(tx_sa);
		kfree_skb(skb);
		return ERR_PTR(-ENOLINK);
	}
	macsec_fill_sectag(hh, secy, pn);
	macsec_set_shortlen(hh, unprotected_len - 2 * ETH_ALEN);

	macsec_fill_iv(iv, secy->sci, pn);

	skb_put(skb, secy->icv_len);

	if (skb->len - ETH_HLEN > macsec_priv(dev)->real_dev->mtu) {
		struct pcpu_secy_stats *secy_stats = this_cpu_ptr(macsec->stats);

		u64_stats_update_begin(&secy_stats->syncp);
		secy_stats->stats.OutPktsTooLong++;
		u64_stats_update_end(&secy_stats->syncp);

		macsec_txsa_put(tx_sa);
		kfree_skb(skb);
		return ERR_PTR(-EINVAL);
	}

	req = aead_request_alloc(tx_sa->key.tfm, GFP_ATOMIC);
	if (!req) {
		macsec_txsa_put(tx_sa);
		kfree_skb(skb);
		return ERR_PTR(-ENOMEM);
	}

	sg_init_table(sg, MAX_SKB_FRAGS + 1);
	skb_to_sgvec(skb, sg, 0, skb->len);

	if (tx_sc->encrypt) {
		int len = skb->len - macsec_hdr_len(tx_sc->send_sci) -
			  secy->icv_len;
		aead_request_set_crypt(req, sg, sg, len, iv);
		aead_request_set_ad(req, macsec_hdr_len(tx_sc->send_sci));
	} else {
		aead_request_set_crypt(req, sg, sg, 0, iv);
		aead_request_set_ad(req, skb->len - secy->icv_len);
	}

	macsec_skb_cb(skb)->req = req;
	macsec_skb_cb(skb)->tx_sa = tx_sa;
	aead_request_set_callback(req, 0, macsec_encrypt_done, skb);

	dev_hold(skb->dev);
	ret = crypto_aead_encrypt(req);
	if (ret == -EINPROGRESS) {
		return ERR_PTR(ret);
	} else if (ret != 0) {
		dev_put(skb->dev);
		kfree_skb(skb);
		aead_request_free(req);
		macsec_txsa_put(tx_sa);
		return ERR_PTR(-EINVAL);
	}

	dev_put(skb->dev);
	aead_request_free(req);
	macsec_txsa_put(tx_sa);

	return skb;
}

static void macsec_reset_skb(struct sk_buff *skb, struct net_device *dev)
{
	skb->pkt_type = PACKET_HOST;
	skb->protocol = eth_type_trans(skb, dev);

	skb_reset_network_header(skb);
	if (!skb_transport_header_was_set(skb))
		skb_reset_transport_header(skb);
	skb_reset_mac_len(skb);
}

static void macsec_finalize_skb(struct sk_buff *skb, u8 icv_len, u8 hdr_len)
{
	memmove(skb->data + hdr_len, skb->data, 2 * ETH_ALEN);
	skb_pull(skb, hdr_len);
	pskb_trim_unique(skb, skb->len - icv_len);
}

static void count_rx(struct net_device *dev, int len)
{
	struct pcpu_sw_netstats *stats = this_cpu_ptr(dev->tstats);

	u64_stats_update_begin(&stats->syncp);
	stats->rx_packets++;
	stats->rx_bytes += len;
	u64_stats_update_end(&stats->syncp);
}

static void macsec_decrypt_done(struct crypto_async_request *base, int err)
{
	struct sk_buff *skb = base->data;
	struct net_device *dev = skb->dev;
	struct macsec_dev *macsec = macsec_priv(dev);
	struct macsec_rx_sa *rx_sa = macsec_skb_cb(skb)->rx_sa;
	int len, ret;

	aead_request_free(macsec_skb_cb(skb)->req);

	rcu_read_lock_bh();
	macsec_finalize_skb(skb, macsec->secy.icv_len,
			    macsec_extra_len(macsec_skb_cb(skb)->has_sci));
	macsec_reset_skb(skb, macsec->secy.netdev);

	macsec_rxsa_put(rx_sa);
	len = skb->len;
	ret = netif_rx(skb);
	if (ret == NET_RX_SUCCESS)
		count_rx(dev, len);
	else
		macsec->secy.netdev->stats.rx_dropped++;

	rcu_read_unlock_bh();

	dev_put(dev);
}

static struct sk_buff *macsec_decrypt(struct sk_buff *skb,
				      struct net_device *dev,
				      struct macsec_rx_sa *rx_sa,
				      sci_t sci,
				      struct macsec_secy *secy)
{
	int ret;
	struct scatterlist sg[MAX_SKB_FRAGS + 1];
	unsigned char iv[GCM_AES_IV_LEN];
	struct aead_request *req;
	struct macsec_eth_header *hdr;
	u16 icv_len = secy->icv_len;

	macsec_skb_cb(skb)->valid = 0;
	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb)
		return NULL;

	req = aead_request_alloc(rx_sa->key.tfm, GFP_ATOMIC);
	if (!req) {
		kfree_skb(skb);
		return NULL;
	}

	hdr = (struct macsec_eth_header *)skb->data;
	macsec_fill_iv(iv, sci, ntohl(hdr->packet_number));

	sg_init_table(sg, MAX_SKB_FRAGS + 1);
	skb_to_sgvec(skb, sg, 0, skb->len);

	if (hdr->tci_an & MACSEC_TCI_E) {
		/* confidentiality: ethernet + macsec header
		 * authenticated, encrypted payload
		 */
		int len = skb->len - macsec_hdr_len(macsec_skb_cb(skb)->has_sci);

		aead_request_set_crypt(req, sg, sg, len, iv);
		aead_request_set_ad(req, macsec_hdr_len(macsec_skb_cb(skb)->has_sci));
		skb = skb_unshare(skb, GFP_ATOMIC);
		if (!skb) {
			aead_request_free(req);
			return NULL;
		}
	} else {
		/* integrity only: all headers + data authenticated */
		aead_request_set_crypt(req, sg, sg, icv_len, iv);
		aead_request_set_ad(req, skb->len - icv_len);
	}

	macsec_skb_cb(skb)->req = req;
	macsec_skb_cb(skb)->rx_sa = rx_sa;
	skb->dev = dev;
	aead_request_set_callback(req, 0, macsec_decrypt_done, skb);

	dev_hold(dev);
	ret = crypto_aead_decrypt(req);
	if (ret == -EINPROGRESS) {
		return NULL;
	} else if (ret != 0) {
		/* decryption/authentication failed
		 * 10.6 if validateFrames is disabled, deliver anyway
		 */
		if (ret != -EBADMSG) {
			kfree_skb(skb);
			skb = NULL;
		}
	} else {
		macsec_skb_cb(skb)->valid = 1;
	}
	dev_put(dev);

	aead_request_free(req);

	return skb;
}

static struct macsec_rx_sc *find_rx_sc(struct macsec_secy *secy, sci_t sci)
{
	struct macsec_rx_sc *rx_sc;

	for_each_rxsc(secy, rx_sc) {
		if (rx_sc->sci == sci)
			return rx_sc;
	}

	return NULL;
}

static struct macsec_rx_sc *find_rx_sc_rtnl(struct macsec_secy *secy, sci_t sci)
{
	struct macsec_rx_sc *rx_sc;

	for_each_rxsc_rtnl(secy, rx_sc) {
		if (rx_sc->sci == sci)
			return rx_sc;
	}

	return NULL;
}

static void handle_not_macsec(struct sk_buff *skb)
{
	struct macsec_rxh_data *rxd = macsec_data_rcu(skb->dev);
	struct macsec_dev *macsec;

	/* 10.6 If the management control validateFrames is not
	 * Strict, frames without a SecTAG are received, counted, and
	 * delivered to the Controlled Port
	 */
	list_for_each_entry_rcu(macsec, &rxd->secys, secys) {
		struct sk_buff *nskb;
		int ret;
		struct pcpu_secy_stats *secy_stats = this_cpu_ptr(macsec->stats);

		if (macsec->secy.validate_frames == MACSEC_VALIDATE_STRICT) {
			u64_stats_update_begin(&secy_stats->syncp);
			secy_stats->stats.InPktsNoTag++;
			u64_stats_update_end(&secy_stats->syncp);
			continue;
		}

		/* deliver on this port */
		nskb = skb_clone(skb, GFP_ATOMIC);
		nskb->dev = macsec->secy.netdev;

		ret = netif_rx(nskb);
		if (ret == NET_RX_SUCCESS) {
			u64_stats_update_begin(&secy_stats->syncp);
			secy_stats->stats.InPktsUntagged++;
			u64_stats_update_end(&secy_stats->syncp);
		} else {
			macsec->secy.netdev->stats.rx_dropped++;
		}
	}
}

static struct macsec_eth_header *macsec_ethhdr(struct sk_buff *skb)
{
	return (struct macsec_eth_header *)skb_mac_header(skb);
}

static rx_handler_result_t macsec_handle_frame(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct net_device *dev = skb->dev;
	struct macsec_eth_header *hdr;
	struct macsec_secy *secy = NULL;
	struct macsec_rx_sc *rx_sc;
	struct macsec_rx_sa *rx_sa;
	struct macsec_rxh_data *rxd;
	struct macsec_dev *macsec;
	sci_t sci;
	u32 pn, lowest_pn;
	bool cbit;
	struct pcpu_rx_sc_stats *rxsc_stats;
	struct pcpu_secy_stats *secy_stats;
	bool pulled_sci;

	rcu_read_lock_bh();

	if (skb_headroom(skb) < ETH_HLEN)
		goto drop_nosa;

	rxd = macsec_data_rcu(skb->dev);

	hdr = macsec_ethhdr(skb);
	if (hdr->eth.h_proto != htons(ETH_P_MACSEC)) {
		handle_not_macsec(skb);
		rcu_read_unlock_bh();

		/* and deliver to the uncontrolled port */
		return RX_HANDLER_PASS;
	}

	skb = skb_unshare(skb, GFP_ATOMIC);
	if (!skb) {
		rcu_read_unlock_bh();
		*pskb = NULL;
		return RX_HANDLER_CONSUMED;
	}

	pulled_sci = pskb_may_pull(skb, macsec_extra_len(true));
	if (!pulled_sci) {
		if (!pskb_may_pull(skb, macsec_extra_len(false)))
			goto drop_nosa;
	}

	hdr = macsec_ethhdr(skb);

	/* Frames with a SecTAG that has the TCI E bit set but the C
	 * bit clear are discarded, as this reserved encoding is used
	 * to identify frames with a SecTAG that are not to be
	 * delivered to the Controlled Port.
	 */
	if ((hdr->tci_an & (MACSEC_TCI_C | MACSEC_TCI_E)) == MACSEC_TCI_E) {
		rcu_read_unlock_bh();
		return RX_HANDLER_PASS;
	}

	/* now, pull the extra length */
	if (hdr->tci_an & MACSEC_TCI_SC) {
		if (!pulled_sci)
			goto drop_nosa;
	}

	/* ethernet header is part of crypto processing */
	skb_push(skb, ETH_HLEN);

	macsec_skb_cb(skb)->has_sci = !!(hdr->tci_an & MACSEC_TCI_SC);
	macsec_skb_cb(skb)->assoc_num = hdr->tci_an & MACSEC_AN_MASK;
	sci = macsec_frame_sci(hdr, macsec_skb_cb(skb)->has_sci);

	list_for_each_entry_rcu(macsec, &rxd->secys, secys) {
		struct macsec_rx_sc *sc = find_rx_sc(&macsec->secy, sci);

		if (sc) {
			secy = &macsec->secy;
			rx_sc = sc;
			break;
		}
	}

	if (!secy)
		goto nosci;

	dev = secy->netdev;
	macsec = macsec_priv(dev);
	secy_stats = this_cpu_ptr(macsec->stats);
	rxsc_stats = this_cpu_ptr(rx_sc->stats);

	if (!macsec_validate_skb(skb, secy->icv_len)) {
		u64_stats_update_begin(&secy_stats->syncp);
		secy_stats->stats.InPktsBadTag++;
		u64_stats_update_end(&secy_stats->syncp);
		goto drop_nosa;
	}

	rx_sa = macsec_rxsa_get(rx_sc->sa[macsec_skb_cb(skb)->assoc_num]);
	if (!rx_sa) {
		/* 10.6.1 if the SA is not in use */

		/* If validateFrames is Strict or the C bit in the
		 * SecTAG is set, discard
		 */
		if (hdr->tci_an & MACSEC_TCI_C ||
		    secy->validate_frames == MACSEC_VALIDATE_STRICT) {
			u64_stats_update_begin(&rxsc_stats->syncp);
			rxsc_stats->stats.InPktsNotUsingSA++;
			u64_stats_update_end(&rxsc_stats->syncp);
			goto drop_nosa;
		}

		/* not Strict, the frame (with the SecTAG and ICV
		 * removed) is delivered to the Controlled Port.
		 */
		u64_stats_update_begin(&rxsc_stats->syncp);
		rxsc_stats->stats.InPktsUnusedSA++;
		u64_stats_update_end(&rxsc_stats->syncp);
		goto deliver;
	}

	pn = ntohl(hdr->packet_number);
	if (secy->replay_protect) {
		bool late;

		spin_lock(&rx_sa->lock);
		late = rx_sa->next_pn >= secy->replay_window &&
		       pn < (rx_sa->next_pn - secy->replay_window);
		spin_unlock(&rx_sa->lock);

		if (late) {
			u64_stats_update_begin(&rxsc_stats->syncp);
			rxsc_stats->stats.InPktsLate++;
			u64_stats_update_end(&rxsc_stats->syncp);
			goto drop;
		}
	}

	/* Disabled && !changed text => skip validation */
	if (hdr->tci_an & MACSEC_TCI_C ||
	    secy->validate_frames != MACSEC_VALIDATE_DISABLED)
		skb = macsec_decrypt(skb, dev, rx_sa, sci, secy);

	if (!skb) {
		macsec_rxsa_put(rx_sa);
		rcu_read_unlock_bh();
		*pskb = NULL;
		return RX_HANDLER_CONSUMED;
	}

	spin_lock(&rx_sa->lock);
	if (rx_sa->next_pn >= secy->replay_window)
		lowest_pn = rx_sa->next_pn - secy->replay_window;
	else
		lowest_pn = 0;

	if (secy->replay_protect && pn < lowest_pn) {
		spin_unlock(&rx_sa->lock);
		pr_debug("packet_number too small: %u < %u\n", pn, lowest_pn);
		u64_stats_update_begin(&rxsc_stats->syncp);
		rxsc_stats->stats.InPktsLate++;
		u64_stats_update_end(&rxsc_stats->syncp);
		goto drop;
	}

	if (secy->validate_frames != MACSEC_VALIDATE_DISABLED) {
		u64_stats_update_begin(&rxsc_stats->syncp);
		if (hdr->tci_an & MACSEC_TCI_E)
			rxsc_stats->stats.InOctetsDecrypted += skb->len;
		else
			rxsc_stats->stats.InOctetsValidated += skb->len;
		u64_stats_update_end(&rxsc_stats->syncp);
	}

	if (!macsec_skb_cb(skb)->valid) {
		spin_unlock(&rx_sa->lock);

		/* 10.6.5 */
		if (hdr->tci_an & MACSEC_TCI_C ||
		    secy->validate_frames == MACSEC_VALIDATE_STRICT) {
			u64_stats_update_begin(&rxsc_stats->syncp);
			rxsc_stats->stats.InPktsNotValid++;
			u64_stats_update_end(&rxsc_stats->syncp);
			goto drop;
		}

		u64_stats_update_begin(&rxsc_stats->syncp);
		if (secy->validate_frames == MACSEC_VALIDATE_CHECK) {
			rxsc_stats->stats.InPktsInvalid++;
			this_cpu_inc(rx_sa->stats->InPktsInvalid);
		} else if (pn < lowest_pn) {
			rxsc_stats->stats.InPktsDelayed++;
		} else {
			rxsc_stats->stats.InPktsUnchecked++;
		}
		u64_stats_update_end(&rxsc_stats->syncp);
	} else {
		u64_stats_update_begin(&rxsc_stats->syncp);
		if (pn < lowest_pn) {
			rxsc_stats->stats.InPktsDelayed++;
		} else {
			rxsc_stats->stats.InPktsOK++;
			this_cpu_inc(rx_sa->stats->InPktsOK);
		}
		u64_stats_update_end(&rxsc_stats->syncp);

		if (pn >= rx_sa->next_pn)
			rx_sa->next_pn = pn + 1;
		spin_unlock(&rx_sa->lock);
	}

deliver:
	macsec_finalize_skb(skb, secy->icv_len,
			    macsec_extra_len(macsec_skb_cb(skb)->has_sci));
	macsec_reset_skb(skb, secy->netdev);

	macsec_rxsa_put(rx_sa);
	count_rx(dev, skb->len);

	rcu_read_unlock_bh();

	*pskb = skb;
	return RX_HANDLER_ANOTHER;

drop:
	macsec_rxsa_put(rx_sa);
drop_nosa:
	rcu_read_unlock_bh();
	kfree_skb(skb);
	*pskb = NULL;
	return RX_HANDLER_CONSUMED;

nosci:
	/* 10.6.1 if the SC is not found */
	cbit = !!(hdr->tci_an & MACSEC_TCI_C);
	if (!cbit)
		macsec_finalize_skb(skb, DEFAULT_ICV_LEN,
				    macsec_extra_len(macsec_skb_cb(skb)->has_sci));

	list_for_each_entry_rcu(macsec, &rxd->secys, secys) {
		struct sk_buff *nskb;
		int ret;

		secy_stats = this_cpu_ptr(macsec->stats);

		/* If validateFrames is Strict or the C bit in the
		 * SecTAG is set, discard
		 */
		if (cbit ||
		    macsec->secy.validate_frames == MACSEC_VALIDATE_STRICT) {
			u64_stats_update_begin(&secy_stats->syncp);
			secy_stats->stats.InPktsNoSCI++;
			u64_stats_update_end(&secy_stats->syncp);
			continue;
		}

		/* not strict, the frame (with the SecTAG and ICV
		 * removed) is delivered to the Controlled Port.
		 */
		nskb = skb_clone(skb, GFP_ATOMIC);
		macsec_reset_skb(nskb, macsec->secy.netdev);

		ret = netif_rx(nskb);
		if (ret == NET_RX_SUCCESS) {
			u64_stats_update_begin(&secy_stats->syncp);
			secy_stats->stats.InPktsUnknownSCI++;
			u64_stats_update_end(&secy_stats->syncp);
		} else {
			macsec->secy.netdev->stats.rx_dropped++;
		}
	}

	rcu_read_unlock_bh();
	*pskb = skb;
	return RX_HANDLER_PASS;
}

static struct crypto_aead *macsec_alloc_tfm(char *key, int key_len, int icv_len)
{
	struct crypto_aead *tfm;
	int ret;

	tfm = crypto_alloc_aead("gcm(aes)", 0, CRYPTO_ALG_ASYNC);
	if (!tfm || IS_ERR(tfm))
		return NULL;

	ret = crypto_aead_setkey(tfm, key, key_len);
	if (ret < 0) {
		crypto_free_aead(tfm);
		return NULL;
	}

	ret = crypto_aead_setauthsize(tfm, icv_len);
	if (ret < 0) {
		crypto_free_aead(tfm);
		return NULL;
	}

	return tfm;
}

static int init_rx_sa(struct macsec_rx_sa *rx_sa, char *sak, int key_len,
		      int icv_len)
{
	rx_sa->stats = alloc_percpu(struct macsec_rx_sa_stats);
	if (!rx_sa->stats)
		return -1;

	rx_sa->key.tfm = macsec_alloc_tfm(sak, key_len, icv_len);
	if (!rx_sa->key.tfm) {
		free_percpu(rx_sa->stats);
		return -1;
	}

	rx_sa->active = false;
	rx_sa->next_pn = 1;
	atomic_set(&rx_sa->refcnt, 1);
	spin_lock_init(&rx_sa->lock);

	return 0;
}

static void clear_rx_sa(struct macsec_rx_sa *rx_sa)
{
	rx_sa->active = false;

	macsec_rxsa_put(rx_sa);
}

static void free_rx_sc_rcu(struct rcu_head *head)
{
	struct macsec_rx_sc *rx_sc = container_of(head, struct macsec_rx_sc, rcu_head);

	free_percpu(rx_sc->stats);
	kfree(rx_sc);
}

static void free_rx_sc(struct macsec_rx_sc *rx_sc)
{
	int i;

	for (i = 0; i < 4; i++) {
		struct macsec_rx_sa *sa = rtnl_dereference(rx_sc->sa[i]);

		RCU_INIT_POINTER(rx_sc->sa[i], NULL);
		if (sa)
			clear_rx_sa(sa);
	}

	call_rcu(&rx_sc->rcu_head, free_rx_sc_rcu);
}

static struct macsec_rx_sc *del_rx_sc(struct macsec_secy *secy, sci_t sci)
{
	struct macsec_rx_sc *rx_sc, __rcu **rx_scp;

	for (rx_scp = &secy->rx_sc, rx_sc = rtnl_dereference(*rx_scp);
	     rx_sc;
	     rx_scp = &rx_sc->next, rx_sc = rtnl_dereference(*rx_scp)) {
		if (rx_sc->sci == sci) {
			if (rx_sc->active)
				secy->n_rx_sc--;
			rcu_assign_pointer(*rx_scp, rx_sc->next);
			return rx_sc;
		}
	}

	return NULL;
}

static struct macsec_rx_sc *create_rx_sc(struct net_device *dev, sci_t sci)
{
	struct macsec_rx_sc *rx_sc;
	struct macsec_dev *macsec;
	struct net_device *real_dev = macsec_priv(dev)->real_dev;
	struct macsec_rxh_data *rxd = macsec_data_rtnl(real_dev);
	struct macsec_secy *secy;

	list_for_each_entry(macsec, &rxd->secys, secys) {
		if (find_rx_sc_rtnl(&macsec->secy, sci))
			return ERR_PTR(-EEXIST);
	}

	rx_sc = kzalloc(sizeof(*rx_sc), GFP_KERNEL);
	if (!rx_sc)
		return ERR_PTR(-ENOMEM);

	rx_sc->stats = netdev_alloc_pcpu_stats(struct pcpu_rx_sc_stats);
	if (!rx_sc->stats) {
		kfree(rx_sc);
		return ERR_PTR(-ENOMEM);
	}

	rx_sc->sci = sci;
	rx_sc->active = true;

	secy = &macsec_priv(dev)->secy;
	rcu_assign_pointer(rx_sc->next, secy->rx_sc);
	rcu_assign_pointer(secy->rx_sc, rx_sc);

	if (rx_sc->active)
		secy->n_rx_sc++;

	return rx_sc;
}

static int init_tx_sa(struct macsec_tx_sa *tx_sa, char *sak, int key_len,
		      int icv_len)
{
	tx_sa->stats = alloc_percpu(struct macsec_tx_sa_stats);
	if (!tx_sa->stats)
		return -1;

	tx_sa->key.tfm = macsec_alloc_tfm(sak, key_len, icv_len);
	if (!tx_sa->key.tfm) {
		free_percpu(tx_sa->stats);
		return -1;
	}

	tx_sa->active = false;
	atomic_set(&tx_sa->refcnt, 1);
	spin_lock_init(&tx_sa->lock);

	return 0;
}

static void clear_tx_sa(struct macsec_tx_sa *tx_sa)
{
	tx_sa->active = false;

	macsec_txsa_put(tx_sa);
}

static struct genl_family macsec_fam = {
	.id		= GENL_ID_GENERATE,
	.name		= MACSEC_GENL_NAME,
	.hdrsize	= 0,
	.version	= MACSEC_GENL_VERSION,
	.maxattr	= MACSEC_ATTR_MAX,
	.netnsok	= true,
};

static struct net_device *get_dev_from_nl(struct net *net,
					  struct nlattr **attrs)
{
	int ifindex = nla_get_u32(attrs[MACSEC_ATTR_IFINDEX]);
	struct net_device *dev;

	dev = __dev_get_by_index(net, ifindex);
	if (!dev)
		return ERR_PTR(-ENODEV);

	if (!netif_is_macsec(dev))
		return ERR_PTR(-ENODEV);

	return dev;
}

static sci_t nla_get_sci(const struct nlattr *nla)
{
	return (__force sci_t)nla_get_u64(nla);
}

static int nla_put_sci(struct sk_buff *skb, int attrtype, sci_t value)
{
	return nla_put_u64(skb, attrtype, (__force u64)value);
}

static struct macsec_tx_sa *get_txsa_from_nl(struct net *net,
					     struct nlattr **attrs,
					     struct net_device **devp,
					     struct macsec_secy **secyp,
					     struct macsec_tx_sc **scp,
					     u8 *assoc_num)
{
	struct net_device *dev;
	struct macsec_secy *secy;
	struct macsec_tx_sc *tx_sc;
	struct macsec_tx_sa *tx_sa;

	if (!attrs[MACSEC_ATTR_AN])
		return ERR_PTR(-EINVAL);

	*assoc_num = nla_get_u8(attrs[MACSEC_ATTR_AN]);

	dev = get_dev_from_nl(net, attrs);
	if (IS_ERR(dev))
		return ERR_CAST(dev);

	if (*assoc_num > 3)
		return ERR_PTR(-EINVAL);

	secy = &macsec_priv(dev)->secy;
	tx_sc = &secy->tx_sc;

	tx_sa = rtnl_dereference(tx_sc->sa[*assoc_num]);
	if (!tx_sa)
		return ERR_PTR(-ENODEV);

	*devp = dev;
	*scp = tx_sc;
	*secyp = secy;
	return tx_sa;
}

static struct macsec_rx_sc *get_rxsc_from_nl(struct net *net,
					     struct nlattr **attrs,
					     struct net_device **devp,
					     struct macsec_secy **secyp)
{
	struct net_device *dev;
	struct macsec_secy *secy;
	struct macsec_rx_sc *rx_sc;
	sci_t sci;

	dev = get_dev_from_nl(net, attrs);
	if (IS_ERR(dev))
		return ERR_CAST(dev);

	secy = &macsec_priv(dev)->secy;

	if (!attrs[MACSEC_ATTR_SCI])
		return ERR_PTR(-EINVAL);

	sci = nla_get_sci(attrs[MACSEC_ATTR_SCI]);
	rx_sc = find_rx_sc_rtnl(secy, sci);
	if (!rx_sc)
		return ERR_PTR(-ENODEV);

	*secyp = secy;
	*devp = dev;

	return rx_sc;
}

static struct macsec_rx_sa *get_rxsa_from_nl(struct net *net,
					     struct nlattr **attrs,
					     struct net_device **devp,
					     struct macsec_secy **secyp,
					     struct macsec_rx_sc **scp,
					     u8 *assoc_num)
{
	struct macsec_rx_sc *rx_sc;
	struct macsec_rx_sa *rx_sa;

	if (!attrs[MACSEC_ATTR_AN])
		return ERR_PTR(-EINVAL);

	*assoc_num = nla_get_u8(attrs[MACSEC_ATTR_AN]);
	if (*assoc_num > 3)
		return ERR_PTR(-EINVAL);

	rx_sc = get_rxsc_from_nl(net, attrs, devp, secyp);
	if (IS_ERR(rx_sc))
		return ERR_CAST(rx_sc);

	rx_sa = rtnl_dereference(rx_sc->sa[*assoc_num]);
	if (!rx_sa)
		return ERR_PTR(-ENODEV);

	*scp = rx_sc;
	return rx_sa;
}

static bool validate_add_rxsa(struct nlattr **attrs)
{
	if (!attrs[MACSEC_ATTR_IFINDEX] ||
	    (!attrs[MACSEC_ATTR_SCI] && !attrs[MACSEC_ATTR_PORT]) ||
	    !attrs[MACSEC_ATTR_AN] ||
	    !attrs[MACSEC_ATTR_KEY] || !attrs[MACSEC_ATTR_KEYID])
		return false;

	if (nla_get_u8(attrs[MACSEC_ATTR_AN]) > 3)
		return false;

	if (attrs[MACSEC_ATTR_PN] && nla_get_u32(attrs[MACSEC_ATTR_PN]) == 0)
		return false;

	if (attrs[MACSEC_ATTR_SA_ACTIVE]) {
		if (nla_get_u8(attrs[MACSEC_ATTR_SA_ACTIVE]) > 1)
			return false;
	}

	return true;
}

static int macsec_add_rxsa(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	struct nlattr **attrs = info->attrs;
	struct macsec_secy *secy;
	struct macsec_rx_sc *rx_sc;
	struct macsec_rx_sa *rx_sa;
	unsigned char assoc_num;

	if (!validate_add_rxsa(attrs))
		return -EINVAL;

	rtnl_lock();
	rx_sc = get_rxsc_from_nl(genl_info_net(info), attrs, &dev, &secy);
	if (IS_ERR(rx_sc)) {
		rtnl_unlock();
		return PTR_ERR(rx_sc);
	}

	assoc_num = nla_get_u8(attrs[MACSEC_ATTR_AN]);

	if (nla_len(attrs[MACSEC_ATTR_KEY]) != secy->key_len) {
		pr_notice("macsec: nl: add_rxsa: bad key length: %d != %d\n",
			  nla_len(attrs[MACSEC_ATTR_KEY]), secy->key_len);
		rtnl_unlock();
		return -EINVAL;
	}

	rx_sa = rtnl_dereference(rx_sc->sa[assoc_num]);
	if (rx_sa) {
		rtnl_unlock();
		return -EBUSY;
	}

	rx_sa = kmalloc(sizeof(*rx_sa), GFP_KERNEL);
	if (init_rx_sa(rx_sa, nla_data(attrs[MACSEC_ATTR_KEY]), secy->key_len,
		       secy->icv_len)) {
		rtnl_unlock();
		return -ENOMEM;
	}

	if (attrs[MACSEC_ATTR_PN]) {
		spin_lock_bh(&rx_sa->lock);
		rx_sa->next_pn = nla_get_u32(attrs[MACSEC_ATTR_PN]);
		spin_unlock_bh(&rx_sa->lock);
	}

	if (attrs[MACSEC_ATTR_SA_ACTIVE])
		rx_sa->active = !!nla_get_u8(attrs[MACSEC_ATTR_SA_ACTIVE]);

	rx_sa->key.id = nla_get_u64(attrs[MACSEC_ATTR_KEYID]);
	rcu_assign_pointer(rx_sc->sa[assoc_num], rx_sa);

	rtnl_unlock();

	return 0;
}

static bool validate_add_rxsc(struct nlattr **attrs)
{
	if (!attrs[MACSEC_ATTR_IFINDEX] ||
	    !attrs[MACSEC_ATTR_SCI])
		return false;

	if (attrs[MACSEC_ATTR_SC_ACTIVE]) {
		if (nla_get_u8(attrs[MACSEC_ATTR_SC_ACTIVE]) > 1)
			return false;
	}

	return true;
}

static int macsec_add_rxsc(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	sci_t sci = MACSEC_UNDEF_SCI;
	struct nlattr **attrs = info->attrs;
	struct macsec_rx_sc *rx_sc;

	if (!validate_add_rxsc(attrs))
		return -EINVAL;

	rtnl_lock();
	dev = get_dev_from_nl(genl_info_net(info), attrs);
	if (IS_ERR(dev)) {
		rtnl_unlock();
		return PTR_ERR(dev);
	}

	sci = nla_get_sci(attrs[MACSEC_ATTR_SCI]);

	rx_sc = create_rx_sc(dev, sci);
	if (IS_ERR(rx_sc)) {
		rtnl_unlock();
		return PTR_ERR(rx_sc);
	}

	if (attrs[MACSEC_ATTR_SC_ACTIVE])
		rx_sc->active = !!nla_get_u8(attrs[MACSEC_ATTR_SC_ACTIVE]);

	rtnl_unlock();

	return 0;
}

static bool validate_add_txsa(struct nlattr **attrs)
{
	if (!attrs[MACSEC_ATTR_IFINDEX] ||
	    !attrs[MACSEC_ATTR_AN] || !attrs[MACSEC_ATTR_PN] ||
	    !attrs[MACSEC_ATTR_KEY] || !attrs[MACSEC_ATTR_KEYID])
		return false;

	if (nla_get_u8(attrs[MACSEC_ATTR_AN]) > 3)
		return false;

	if (nla_get_u32(attrs[MACSEC_ATTR_PN]) == 0)
		return false;

	if (attrs[MACSEC_ATTR_SA_ACTIVE]) {
		if (nla_get_u8(attrs[MACSEC_ATTR_SA_ACTIVE]) > 1)
			return false;
	}

	return true;
}

static int macsec_add_txsa(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	struct nlattr **attrs = info->attrs;
	struct macsec_secy *secy;
	struct macsec_tx_sc *tx_sc;
	struct macsec_tx_sa *tx_sa;
	unsigned char assoc_num;

	if (!validate_add_txsa(attrs))
		return -EINVAL;

	rtnl_lock();
	dev = get_dev_from_nl(genl_info_net(info), attrs);
	if (IS_ERR(dev)) {
		rtnl_unlock();
		return PTR_ERR(dev);
	}

	secy = &macsec_priv(dev)->secy;
	tx_sc = &secy->tx_sc;

	assoc_num = nla_get_u8(attrs[MACSEC_ATTR_AN]);

	if (nla_len(attrs[MACSEC_ATTR_KEY]) != secy->key_len) {
		pr_notice("macsec: nl: add_txsa: bad key length: %d != %d\n",
			  nla_len(attrs[MACSEC_ATTR_KEY]), secy->key_len);
		rtnl_unlock();
		return -EINVAL;
	}

	tx_sa = rtnl_dereference(tx_sc->sa[assoc_num]);
	if (tx_sa) {
		rtnl_unlock();
		return -EBUSY;
	}

	tx_sa = kmalloc(sizeof(*tx_sa), GFP_KERNEL);
	if (!tx_sa || init_tx_sa(tx_sa, nla_data(attrs[MACSEC_ATTR_KEY]),
				 secy->key_len, secy->icv_len)) {
		rtnl_unlock();
		return -ENOMEM;
	}

	tx_sa->key.id = nla_get_u64(attrs[MACSEC_ATTR_KEYID]);

	spin_lock_bh(&tx_sa->lock);
	tx_sa->next_pn = nla_get_u32(attrs[MACSEC_ATTR_PN]);
	spin_unlock_bh(&tx_sa->lock);

	if (attrs[MACSEC_ATTR_SA_ACTIVE])
		tx_sa->active = !!nla_get_u8(attrs[MACSEC_ATTR_SA_ACTIVE]);

	if (assoc_num == tx_sc->encoding_sa && tx_sa->active)
		secy->operational = true;

	rcu_assign_pointer(tx_sc->sa[assoc_num], tx_sa);

	rtnl_unlock();

	return 0;
}

static int macsec_del_rxsa(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	struct macsec_secy *secy;
	struct macsec_rx_sc *rx_sc;
	struct macsec_rx_sa *rx_sa;
	u8 assoc_num;

	rtnl_lock();
	rx_sa = get_rxsa_from_nl(genl_info_net(info), info->attrs, &dev, &secy,
				 &rx_sc, &assoc_num);
	if (IS_ERR(rx_sa)) {
		rtnl_unlock();
		return PTR_ERR(rx_sa);
	}

	if (rx_sa->active) {
		rtnl_unlock();
		return -EBUSY;
	}

	RCU_INIT_POINTER(rx_sc->sa[assoc_num], NULL);
	clear_rx_sa(rx_sa);

	rtnl_unlock();

	return 0;
}

static bool validate_del_rxsc(struct nlattr **attrs)
{
	return attrs[MACSEC_ATTR_IFINDEX] && attrs[MACSEC_ATTR_SCI];
}

static int macsec_del_rxsc(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	struct macsec_secy *secy;
	struct macsec_rx_sc *rx_sc;
	sci_t sci;

	if (!validate_del_rxsc(info->attrs))
		return -EINVAL;

	rtnl_lock();
	dev = get_dev_from_nl(genl_info_net(info), info->attrs);
	if (IS_ERR(dev)) {
		rtnl_unlock();
		return PTR_ERR(dev);
	}

	secy = &macsec_priv(dev)->secy;
	sci = nla_get_sci(info->attrs[MACSEC_ATTR_SCI]);

	rx_sc = del_rx_sc(secy, sci);
	if (!rx_sc) {
		rtnl_unlock();
		return -ENODEV;
	}

	free_rx_sc(rx_sc);
	rtnl_unlock();

	return 0;
}

static int macsec_del_txsa(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	struct macsec_secy *secy;
	struct macsec_tx_sc *tx_sc;
	struct macsec_tx_sa *tx_sa;
	u8 assoc_num;

	rtnl_lock();
	tx_sa = get_txsa_from_nl(genl_info_net(info), info->attrs, &dev, &secy,
				 &tx_sc, &assoc_num);
	if (IS_ERR(tx_sa)) {
		rtnl_unlock();
		return PTR_ERR(tx_sa);
	}

	if (tx_sa->active) {
		rtnl_unlock();
		return -EBUSY;
	}

	RCU_INIT_POINTER(tx_sc->sa[assoc_num], NULL);
	clear_tx_sa(tx_sa);

	rtnl_unlock();

	return 0;
}

static int macsec_upd_txsa(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **attrs = info->attrs;
	struct net_device *dev;
	struct macsec_secy *secy;
	struct macsec_tx_sc *tx_sc;
	struct macsec_tx_sa *tx_sa;
	u8 assoc_num;

	rtnl_lock();
	tx_sa = get_txsa_from_nl(genl_info_net(info), info->attrs, &dev, &secy,
				 &tx_sc, &assoc_num);
	if (IS_ERR(tx_sa)) {
		rtnl_unlock();
		return PTR_ERR(tx_sa);
	}

	if (attrs[MACSEC_ATTR_PN]) {
		spin_lock_bh(&tx_sa->lock);
		tx_sa->next_pn = nla_get_u32(attrs[MACSEC_ATTR_PN]);
		spin_unlock_bh(&tx_sa->lock);
	}

	if (attrs[MACSEC_ATTR_SA_ACTIVE])
		tx_sa->active = nla_get_u8(attrs[MACSEC_ATTR_SA_ACTIVE]);

	if (assoc_num == tx_sc->encoding_sa)
		secy->operational = tx_sa->active;

	rtnl_unlock();

	return 0;
}

static int macsec_upd_rxsa(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **attrs = info->attrs;
	struct net_device *dev;
	struct macsec_secy *secy;
	struct macsec_rx_sc *rx_sc;
	struct macsec_rx_sa *rx_sa;
	u8 assoc_num;

	rtnl_lock();
	rx_sa = get_rxsa_from_nl(genl_info_net(info), info->attrs, &dev, &secy,
				 &rx_sc, &assoc_num);
	if (IS_ERR(rx_sa)) {
		rtnl_unlock();
		return PTR_ERR(rx_sa);
	}

	if (attrs[MACSEC_ATTR_PN]) {
		spin_lock_bh(&rx_sa->lock);
		rx_sa->next_pn = nla_get_u32(attrs[MACSEC_ATTR_PN]);
		spin_unlock_bh(&rx_sa->lock);
	}

	if (attrs[MACSEC_ATTR_SA_ACTIVE])
		rx_sa->active = nla_get_u8(attrs[MACSEC_ATTR_SA_ACTIVE]);

	rtnl_unlock();
	return 0;
}

static int macsec_upd_rxsc(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **attrs = info->attrs;
	struct net_device *dev;
	struct macsec_secy *secy;
	struct macsec_rx_sc *rx_sc;

	if (!validate_del_rxsc(info->attrs))
		return -EINVAL;

	rtnl_lock();
	rx_sc = get_rxsc_from_nl(genl_info_net(info), info->attrs, &dev, &secy);
	if (IS_ERR(rx_sc)) {
		rtnl_unlock();
		return PTR_ERR(rx_sc);
	}

	if (attrs[MACSEC_ATTR_SC_ACTIVE]) {
		bool new = !!nla_get_u8(attrs[MACSEC_ATTR_SC_ACTIVE]);

		if (rx_sc->active != new)
			secy->n_rx_sc += new ? 1 : -1;

		rx_sc->active = new;
	}

	rtnl_unlock();

	return 0;
}

static void copy_tx_sa_stats(struct macsec_tx_sa_stats *sum,
			     struct macsec_tx_sa_stats __percpu *pstats)
{
	int cpu;

	memset(sum, 0, sizeof(*sum));
	for_each_possible_cpu(cpu) {
		const struct macsec_tx_sa_stats *stats = per_cpu_ptr(pstats, cpu);

		sum->OutPktsProtected += stats->OutPktsProtected;
		sum->OutPktsEncrypted += stats->OutPktsEncrypted;
	}
}

static void copy_rx_sa_stats(struct macsec_rx_sa_stats *sum,
			     struct macsec_rx_sa_stats __percpu *pstats)
{
	int cpu;

	memset(sum, 0, sizeof(*sum));
	for_each_possible_cpu(cpu) {
		const struct macsec_rx_sa_stats *stats = per_cpu_ptr(pstats, cpu);

		sum->InPktsOK         += stats->InPktsOK;
		sum->InPktsInvalid    += stats->InPktsInvalid;
		sum->InPktsNotValid   += stats->InPktsNotValid;
		sum->InPktsNotUsingSA += stats->InPktsNotUsingSA;
		sum->InPktsUnusedSA   += stats->InPktsUnusedSA;
	}
}

static void copy_rx_sc_stats(struct macsec_rx_sc_stats *sum,
			     struct pcpu_rx_sc_stats __percpu *pstats)
{
	int cpu;

	memset(sum, 0, sizeof(*sum));
	for_each_possible_cpu(cpu) {
		const struct pcpu_rx_sc_stats *stats;
		struct macsec_rx_sc_stats tmp;
		unsigned int start;

		stats = per_cpu_ptr(pstats, cpu);
		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			memcpy(&tmp, &stats->stats, sizeof(tmp));
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));

		sum->InOctetsValidated += tmp.InOctetsValidated;
		sum->InOctetsDecrypted += tmp.InOctetsDecrypted;
		sum->InPktsUnchecked   += tmp.InPktsUnchecked;
		sum->InPktsDelayed     += tmp.InPktsDelayed;
		sum->InPktsOK          += tmp.InPktsOK;
		sum->InPktsInvalid     += tmp.InPktsInvalid;
		sum->InPktsLate        += tmp.InPktsLate;
		sum->InPktsNotValid    += tmp.InPktsNotValid;
		sum->InPktsNotUsingSA  += tmp.InPktsNotUsingSA;
		sum->InPktsUnusedSA    += tmp.InPktsUnusedSA;
	}
}

static void copy_tx_sc_stats(struct macsec_tx_sc_stats *sum,
			     struct pcpu_tx_sc_stats __percpu *pstats)
{
	int cpu;

	memset(sum, 0, sizeof(*sum));
	for_each_possible_cpu(cpu) {
		const struct pcpu_tx_sc_stats *stats;
		struct macsec_tx_sc_stats tmp;
		unsigned int start;

		stats = per_cpu_ptr(pstats, cpu);
		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			memcpy(&tmp, &stats->stats, sizeof(tmp));
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));

		sum->OutPktsProtected   += tmp.OutPktsProtected;
		sum->OutPktsEncrypted   += tmp.OutPktsEncrypted;
		sum->OutOctetsProtected += tmp.OutOctetsProtected;
		sum->OutOctetsEncrypted += tmp.OutOctetsEncrypted;
	}
}

static void copy_secy_stats(struct macsec_dev_stats *sum,
			    struct pcpu_secy_stats __percpu *pstats)
{
	int cpu;

	memset(sum, 0, sizeof(*sum));
	for_each_possible_cpu(cpu) {
		const struct pcpu_secy_stats *stats;
		struct macsec_dev_stats tmp;
		unsigned int start;

		stats = per_cpu_ptr(pstats, cpu);
		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			memcpy(&tmp, &stats->stats, sizeof(tmp));
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));

		sum->OutPktsUntagged  += tmp.OutPktsUntagged;
		sum->InPktsUntagged   += tmp.InPktsUntagged;
		sum->OutPktsTooLong   += tmp.OutPktsTooLong;
		sum->InPktsNoTag      += tmp.InPktsNoTag;
		sum->InPktsBadTag     += tmp.InPktsBadTag;
		sum->InPktsUnknownSCI += tmp.InPktsUnknownSCI;
		sum->InPktsNoSCI      += tmp.InPktsNoSCI;
		sum->InPktsOverrun    += tmp.InPktsOverrun;
	}
}

static int dump_secy(struct macsec_secy *secy, struct net_device *dev,
		     struct sk_buff *skb, struct netlink_callback *cb)
{
	struct macsec_rx_sc *rx_sc;
	struct macsec_tx_sc *tx_sc = &secy->tx_sc;
	struct nlattr *txsa_list, *rxsc_list;
	int i;
	void *hdr;
	struct nlattr *attr;

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &macsec_fam, NLM_F_MULTI, MACSEC_CMD_GET_TXSC);
	if (!hdr)
		return -EMSGSIZE;

	rtnl_lock();

	if (nla_put_u32(skb, MACSEC_ATTR_IFINDEX, dev->ifindex) ||
	    nla_put_sci(skb, MACSEC_ATTR_SCI, secy->sci) ||
	    nla_put_u64(skb, MACSEC_ATTR_CIPHER_SUITE, DEFAULT_CIPHER_ID) ||
	    nla_put_u8(skb, MACSEC_ATTR_ICV_LEN, secy->icv_len) ||
	    nla_put_u8(skb, MACSEC_ATTR_OPER, secy->operational) ||
	    nla_put_u8(skb, MACSEC_ATTR_PROTECT, secy->protect_frames) ||
	    nla_put_u8(skb, MACSEC_ATTR_REPLAY, secy->replay_protect) ||
	    nla_put_u8(skb, MACSEC_ATTR_VALIDATE, secy->validate_frames) ||
	    nla_put_u8(skb, MACSEC_ATTR_ENCRYPT, tx_sc->encrypt) ||
	    nla_put_u8(skb, MACSEC_ATTR_INC_SCI, tx_sc->send_sci) ||
	    nla_put_u8(skb, MACSEC_ATTR_ES, tx_sc->end_station) ||
	    nla_put_u8(skb, MACSEC_ATTR_SCB, tx_sc->scb) ||
	    nla_put_u8(skb, MACSEC_ATTR_ENCODING_SA, tx_sc->encoding_sa))
		goto nla_put_failure;

	attr = nla_reserve(skb, MACSEC_TXSC_STATS,
			   sizeof(struct macsec_tx_sc_stats));
	if (!attr)
		goto nla_put_failure;
	copy_tx_sc_stats(nla_data(attr), tx_sc->stats);

	attr = nla_reserve(skb, MACSEC_SECY_STATS,
			   sizeof(struct macsec_dev_stats));
	if (!attr)
		goto nla_put_failure;
	copy_secy_stats(nla_data(attr), macsec_priv(dev)->stats);

	if (secy->replay_protect) {
		if (nla_put_u32(skb, MACSEC_ATTR_WINDOW, secy->replay_window))
			goto nla_put_failure;
	}

	txsa_list = nla_nest_start(skb, MACSEC_TXSA_LIST);
	if (!txsa_list)
		goto nla_put_failure;
	for (i = 0; i < 4; i++) {
		struct macsec_tx_sa *tx_sa = rtnl_dereference(tx_sc->sa[i]);
		struct nlattr *txsa_nest;

		if (!tx_sa)
			continue;

		txsa_nest = nla_nest_start(skb, MACSEC_SA);
		if (!txsa_nest) {
			nla_nest_cancel(skb, txsa_list);
			goto nla_put_failure;
		}

		if (nla_put_u8(skb, MACSEC_ATTR_SA_AN, i) ||
		    nla_put_u32(skb, MACSEC_ATTR_SA_PN, tx_sa->next_pn) ||
		    nla_put_u64(skb, MACSEC_ATTR_SA_KEYID, tx_sa->key.id) ||
		    nla_put_u8(skb, MACSEC_ATTR_SA_STATE, tx_sa->active)) {
			nla_nest_cancel(skb, txsa_nest);
			nla_nest_cancel(skb, txsa_list);
			goto nla_put_failure;
		}

		attr = nla_reserve(skb, MACSEC_SA_STATS,
				   sizeof(struct macsec_tx_sa_stats));
		if (!attr)
			goto nla_put_failure;
		copy_tx_sa_stats(nla_data(attr), tx_sa->stats);

		nla_nest_end(skb, txsa_nest);
	}
	nla_nest_end(skb, txsa_list);

	rxsc_list = nla_nest_start(skb, MACSEC_RXSC_LIST);
	if (!rxsc_list)
		goto nla_put_failure;

	for_each_rxsc_rtnl(secy, rx_sc) {
		struct nlattr *rxsa_list;
		struct nlattr *rxsc_nest = nla_nest_start(skb, MACSEC_RXSC);

		if (!rxsc_nest) {
			nla_nest_cancel(skb, rxsc_list);
			goto nla_put_failure;
		}

		if (nla_put_u8(skb, MACSEC_ATTR_SC_STATE, rx_sc->active) ||
		    nla_put_sci(skb, MACSEC_ATTR_SC_SCI, rx_sc->sci)) {
			nla_nest_cancel(skb, rxsc_nest);
			nla_nest_cancel(skb, rxsc_list);
			goto nla_put_failure;
		}

		attr = nla_reserve(skb, MACSEC_RXSC_STATS,
				   sizeof(struct macsec_rx_sc_stats));
		if (!attr)
			goto nla_put_failure;
		copy_rx_sc_stats(nla_data(attr), rx_sc->stats);

		rxsa_list = nla_nest_start(skb, MACSEC_RXSA_LIST);
		if (!rxsa_list) {
			nla_nest_cancel(skb, rxsc_nest);
			nla_nest_cancel(skb, rxsc_list);
			goto nla_put_failure;
		}

		for (i = 0; i < 4; i++) {
			struct macsec_rx_sa *rx_sa = rtnl_dereference(rx_sc->sa[i]);
			struct nlattr *rxsa_nest;

			if (!rx_sa)
				continue;

			rxsa_nest = nla_nest_start(skb, MACSEC_SA);
			if (!rxsa_nest) {
				nla_nest_cancel(skb, rxsa_list);
				nla_nest_cancel(skb, rxsc_nest);
				nla_nest_cancel(skb, rxsc_list);
				goto nla_put_failure;
			}

			attr = nla_reserve(skb, MACSEC_SA_STATS,
					   sizeof(struct macsec_rx_sa_stats));
			if (!attr)
				goto nla_put_failure;
			copy_rx_sa_stats(nla_data(attr), rx_sa->stats);

			if (nla_put_u8(skb, MACSEC_ATTR_SA_AN, i) ||
			    nla_put_u32(skb, MACSEC_ATTR_SA_PN, rx_sa->next_pn) ||
			    nla_put_u64(skb, MACSEC_ATTR_SA_KEYID, rx_sa->key.id) ||
			    nla_put_u8(skb, MACSEC_ATTR_SA_STATE, rx_sa->active)) {
				nla_nest_cancel(skb, rxsa_nest);
				nla_nest_cancel(skb, rxsc_nest);
				nla_nest_cancel(skb, rxsc_list);
				goto nla_put_failure;
			}
			nla_nest_end(skb, rxsa_nest);
		}

		nla_nest_end(skb, rxsa_list);
		nla_nest_end(skb, rxsc_nest);
	}

	nla_nest_end(skb, rxsc_list);

	rtnl_unlock();

	genlmsg_end(skb, hdr);

	return 0;

nla_put_failure:
	rtnl_unlock();
	genlmsg_cancel(skb, hdr);
	return -EMSGSIZE;
}

static int macsec_dump_txsc(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct net_device *dev;
	int dev_idx, d;

	dev_idx = cb->args[0];

	d = 0;
	for_each_netdev(net, dev) {
		struct macsec_secy *secy;

		if (d < dev_idx)
			goto next;

		if (!netif_is_macsec(dev))
			goto next;

		secy = &macsec_priv(dev)->secy;
		if (dump_secy(secy, dev, skb, cb) < 0)
			goto done;
next:
		d++;
	}

done:
	cb->args[0] = d;
	return skb->len;
}

static const struct nla_policy macsec_genl_policy[NUM_MACSEC_ATTR] = {
	[MACSEC_ATTR_IFINDEX] = { .type = NLA_U32 },
	[MACSEC_ATTR_SCI] = { .type = NLA_U64 },
	[MACSEC_ATTR_PN] = { .type = NLA_U32 },
	[MACSEC_ATTR_WINDOW] = { .type = NLA_U32 },
	[MACSEC_ATTR_AN] = { .type = NLA_U8 },
	[MACSEC_ATTR_KEYID] = { .type = NLA_U64 },
	[MACSEC_ATTR_KEY] = { .type = NLA_BINARY,
			      .len = MACSEC_MAX_KEY_LEN, },
	[MACSEC_ATTR_CIPHER_SUITE] = { .type = NLA_U64 },
	[MACSEC_ATTR_ICV_LEN] = { .type = NLA_U8 },
	[MACSEC_ATTR_SC_ACTIVE] = { .type = NLA_U8 },
	[MACSEC_ATTR_SA_ACTIVE] = { .type = NLA_U8 },
	[MACSEC_ATTR_PROTECT] = { .type = NLA_U8 },
	[MACSEC_ATTR_REPLAY] = { .type = NLA_U8 },
	[MACSEC_ATTR_OPER] = { .type = NLA_U8 },
	[MACSEC_ATTR_VALIDATE] = { .type = NLA_U8 },
	[MACSEC_ATTR_ENCRYPT] = { .type = NLA_U8 },
	[MACSEC_ATTR_INC_SCI] = { .type = NLA_U8 },
	[MACSEC_ATTR_ES] = { .type = NLA_U8 },
	[MACSEC_ATTR_SCB] = { .type = NLA_U8 },
};

static const struct genl_ops macsec_genl_ops[] = {
	{
		.cmd = MACSEC_CMD_GET_TXSC,
		.dumpit = macsec_dump_txsc,
		.policy = macsec_genl_policy,
	},
	{
		.cmd = MACSEC_CMD_ADD_RXSC,
		.doit = macsec_add_rxsc,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MACSEC_CMD_DEL_RXSC,
		.doit = macsec_del_rxsc,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MACSEC_CMD_UPD_RXSC,
		.doit = macsec_upd_rxsc,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MACSEC_CMD_ADD_TXSA,
		.doit = macsec_add_txsa,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MACSEC_CMD_DEL_TXSA,
		.doit = macsec_del_txsa,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MACSEC_CMD_UPD_TXSA,
		.doit = macsec_upd_txsa,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MACSEC_CMD_ADD_RXSA,
		.doit = macsec_add_rxsa,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MACSEC_CMD_DEL_RXSA,
		.doit = macsec_del_rxsa,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MACSEC_CMD_UPD_RXSA,
		.doit = macsec_upd_rxsa,
		.policy = macsec_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
};

static netdev_tx_t macsec_start_xmit(struct sk_buff *skb,
				     struct net_device *dev)
{
	struct macsec_dev *macsec = netdev_priv(dev);
	struct macsec_secy *secy = &macsec->secy;
	struct pcpu_secy_stats *secy_stats;
	int ret, len;

	/* 10.5 */
	if (!secy->protect_frames) {
		secy_stats = this_cpu_ptr(macsec->stats);
		u64_stats_update_begin(&secy_stats->syncp);
		secy_stats->stats.OutPktsUntagged++;
		u64_stats_update_end(&secy_stats->syncp);
		len = skb->len;
		ret = dev_queue_xmit(skb);
		count_tx(dev, ret, len);
		return ret;
	}

	if (!secy->operational) {
		kfree_skb(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	skb = macsec_encrypt(skb, dev);
	if (IS_ERR(skb)) {
		if (PTR_ERR(skb) != -EINPROGRESS)
			dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	macsec_count_tx(skb, &macsec->secy.tx_sc, macsec_skb_cb(skb)->tx_sa);

	macsec_encrypt_finish(skb, dev);
	len = skb->len;
	ret = dev_queue_xmit(skb);
	count_tx(dev, ret, len);
	return ret;
}

#define MACSEC_FEATURES \
	(NETIF_F_SG | NETIF_F_HIGHDMA | NETIF_F_FRAGLIST)
static int macsec_dev_init(struct net_device *dev)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct net_device *real_dev = macsec->real_dev;

	dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!dev->tstats)
		return -ENOMEM;

	dev->features = real_dev->features & MACSEC_FEATURES;
	dev->features |= NETIF_F_LLTX | NETIF_F_GSO_SOFTWARE;

	dev->needed_headroom = real_dev->needed_headroom +
			       MACSEC_NEEDED_HEADROOM;
	dev->needed_tailroom = real_dev->needed_tailroom +
			       MACSEC_NEEDED_TAILROOM;

	if (is_zero_ether_addr(dev->dev_addr))
		eth_hw_addr_inherit(dev, real_dev);
	if (is_zero_ether_addr(dev->broadcast))
		memcpy(dev->broadcast, real_dev->broadcast, dev->addr_len);

	return 0;
}

static void macsec_dev_uninit(struct net_device *dev)
{
	free_percpu(dev->tstats);
}

static netdev_features_t macsec_fix_features(struct net_device *dev,
					     netdev_features_t features)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct net_device *real_dev = macsec->real_dev;

	features &= real_dev->features & MACSEC_FEATURES;
	features |= NETIF_F_LLTX | NETIF_F_GSO_SOFTWARE;

	return features;
}

static int macsec_dev_open(struct net_device *dev)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct net_device *real_dev = macsec->real_dev;
	int err;

	if (!(real_dev->flags & IFF_UP))
		return -ENETDOWN;

	err = dev_uc_add(real_dev, dev->dev_addr);
	if (err < 0)
		return err;

	if (dev->flags & IFF_ALLMULTI) {
		err = dev_set_allmulti(real_dev, 1);
		if (err < 0)
			goto del_unicast;
	}

	if (dev->flags & IFF_PROMISC) {
		err = dev_set_promiscuity(real_dev, 1);
		if (err < 0)
			goto clear_allmulti;
	}

	if (netif_carrier_ok(real_dev))
		netif_carrier_on(dev);

	return 0;
clear_allmulti:
	if (dev->flags & IFF_ALLMULTI)
		dev_set_allmulti(real_dev, -1);
del_unicast:
	dev_uc_del(real_dev, dev->dev_addr);
	netif_carrier_off(dev);
	return err;
}

static int macsec_dev_stop(struct net_device *dev)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct net_device *real_dev = macsec->real_dev;

	netif_carrier_off(dev);

	dev_mc_unsync(real_dev, dev);
	dev_uc_unsync(real_dev, dev);

	if (dev->flags & IFF_ALLMULTI)
		dev_set_allmulti(real_dev, -1);

	if (dev->flags & IFF_PROMISC)
		dev_set_promiscuity(real_dev, -1);

	dev_uc_del(real_dev, dev->dev_addr);

	return 0;
}

static void macsec_dev_change_rx_flags(struct net_device *dev, int change)
{
	struct net_device *real_dev = macsec_priv(dev)->real_dev;

	if (!(dev->flags & IFF_UP))
		return;

	if (change & IFF_ALLMULTI)
		dev_set_allmulti(real_dev, dev->flags & IFF_ALLMULTI ? 1 : -1);

	if (change & IFF_PROMISC)
		dev_set_promiscuity(real_dev,
				    dev->flags & IFF_PROMISC ? 1 : -1);
}

static void macsec_dev_set_rx_mode(struct net_device *dev)
{
	struct net_device *real_dev = macsec_priv(dev)->real_dev;

	dev_mc_sync(real_dev, dev);
	dev_uc_sync(real_dev, dev);
}

static int macsec_set_mac_address(struct net_device *dev, void *p)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct net_device *real_dev = macsec->real_dev;
	struct sockaddr *addr = p;
	int err;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	if (!(dev->flags & IFF_UP))
		goto out;

	err = dev_uc_add(real_dev, addr->sa_data);
	if (err < 0)
		return err;

	dev_uc_del(real_dev, dev->dev_addr);

out:
	ether_addr_copy(dev->dev_addr, addr->sa_data);
	return 0;
}

static int macsec_change_mtu(struct net_device *dev, int new_mtu)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	unsigned int extra = macsec->secy.icv_len + macsec_extra_len(true);

	if (macsec->real_dev->mtu - extra < new_mtu)
		return -ERANGE;

	dev->mtu = new_mtu;

	return 0;
}

static struct rtnl_link_stats64 *macsec_get_stats64(struct net_device *dev,
						    struct rtnl_link_stats64 *s)
{
	int cpu;

	if (!dev->tstats)
		return s;

	for_each_possible_cpu(cpu) {
		struct pcpu_sw_netstats *stats;
		struct pcpu_sw_netstats tmp;
		int start;

		stats = per_cpu_ptr(dev->tstats, cpu);
		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			tmp.rx_packets = stats->rx_packets;
			tmp.rx_bytes   = stats->rx_bytes;
			tmp.tx_packets = stats->tx_packets;
			tmp.tx_bytes   = stats->tx_bytes;
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));

		s->rx_packets += tmp.rx_packets;
		s->rx_bytes   += tmp.rx_bytes;
		s->tx_packets += tmp.tx_packets;
		s->tx_bytes   += tmp.tx_bytes;
	}

	s->rx_dropped = dev->stats.rx_dropped;
	s->tx_dropped = dev->stats.tx_dropped;

	return s;
}

static const struct net_device_ops macsec_netdev_ops = {
	.ndo_init		= macsec_dev_init,
	.ndo_uninit		= macsec_dev_uninit,
	.ndo_open		= macsec_dev_open,
	.ndo_stop		= macsec_dev_stop,
	.ndo_fix_features	= macsec_fix_features,
	.ndo_change_mtu		= macsec_change_mtu,
	.ndo_set_rx_mode	= macsec_dev_set_rx_mode,
	.ndo_change_rx_flags	= macsec_dev_change_rx_flags,
	.ndo_set_mac_address	= macsec_set_mac_address,
	.ndo_start_xmit		= macsec_start_xmit,
	.ndo_get_stats64	= macsec_get_stats64,
};

static const struct device_type macsec_type = {
	.name = "macsec",
};

static const struct nla_policy macsec_rtnl_policy[IFLA_MACSEC_MAX + 1] = {
	[IFLA_MACSEC_SCI] = { .type = NLA_U64 },
	[IFLA_MACSEC_PORT] = { .type = NLA_U16 },
	[IFLA_MACSEC_ICV_LEN] = { .type = NLA_U8 },
	[IFLA_MACSEC_CIPHER_SUITE] = { .type = NLA_U64 },
	[IFLA_MACSEC_WINDOW] = { .type = NLA_U32 },
	[IFLA_MACSEC_ENCODING_SA] = { .type = NLA_U8 },
	[IFLA_MACSEC_ENCRYPT] = { .type = NLA_U8 },
	[IFLA_MACSEC_PROTECT] = { .type = NLA_U8 },
	[IFLA_MACSEC_INC_SCI] = { .type = NLA_U8 },
	[IFLA_MACSEC_ES] = { .type = NLA_U8 },
	[IFLA_MACSEC_SCB] = { .type = NLA_U8 },
	[IFLA_MACSEC_REPLAY_PROTECT] = { .type = NLA_U8 },
	[IFLA_MACSEC_VALIDATION] = { .type = NLA_U8 },
};

static void macsec_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->tx_queue_len = 0;
	dev->netdev_ops = &macsec_netdev_ops;
	dev->destructor = free_netdev;

	eth_zero_addr(dev->broadcast);
}

static void macsec_changelink_common(struct net_device *dev,
				     struct nlattr *data[])
{
	struct macsec_secy *secy;
	struct macsec_tx_sc *tx_sc;

	secy = &macsec_priv(dev)->secy;
	tx_sc = &secy->tx_sc;

	if (data[IFLA_MACSEC_ENCODING_SA]) {
		struct macsec_tx_sa *tx_sa;

		tx_sc->encoding_sa = nla_get_u8(data[IFLA_MACSEC_ENCODING_SA]);
		tx_sa = rtnl_dereference(tx_sc->sa[tx_sc->encoding_sa]);

		secy->operational = tx_sa && tx_sa->active;
	}

	if (data[IFLA_MACSEC_WINDOW])
		secy->replay_window = nla_get_u32(data[IFLA_MACSEC_WINDOW]);

	if (data[IFLA_MACSEC_ENCRYPT])
		tx_sc->encrypt = !!nla_get_u8(data[IFLA_MACSEC_ENCRYPT]);

	if (data[IFLA_MACSEC_PROTECT])
		secy->protect_frames = !!nla_get_u8(data[IFLA_MACSEC_PROTECT]);

	if (data[IFLA_MACSEC_INC_SCI])
		tx_sc->send_sci = !!nla_get_u8(data[IFLA_MACSEC_INC_SCI]);

	if (data[IFLA_MACSEC_ES])
		tx_sc->end_station = !!nla_get_u8(data[IFLA_MACSEC_ES]);

	if (data[IFLA_MACSEC_SCB])
		tx_sc->scb = !!nla_get_u8(data[IFLA_MACSEC_SCB]);

	if (data[IFLA_MACSEC_REPLAY_PROTECT])
		secy->replay_protect = !!nla_get_u8(data[IFLA_MACSEC_REPLAY_PROTECT]);

	if (data[IFLA_MACSEC_VALIDATION])
		secy->validate_frames = nla_get_u8(data[IFLA_MACSEC_VALIDATION]);
}

static int macsec_changelink(struct net_device *dev, struct nlattr *tb[],
			     struct nlattr *data[])
{
	if (!data)
		return 0;

	if (data[IFLA_MACSEC_CIPHER_SUITE] ||
	    data[IFLA_MACSEC_ICV_LEN] ||
	    data[IFLA_MACSEC_SCI] ||
	    data[IFLA_MACSEC_PORT])
		return -EINVAL;

	macsec_changelink_common(dev, data);

	return 0;
}

static void macsec_del_dev(struct net_device *dev)
{
	int i;
	struct macsec_dev *macsec = macsec_priv(dev);

	while (macsec->secy.rx_sc) {
		struct macsec_rx_sc *rx_sc = rtnl_dereference(macsec->secy.rx_sc);

		rcu_assign_pointer(macsec->secy.rx_sc, rx_sc->next);
		free_rx_sc(rx_sc);
	}

	for (i = 0; i < 4; i++) {
		struct macsec_tx_sa *sa = rtnl_dereference(macsec->secy.tx_sc.sa[i]);

		if (sa) {
			RCU_INIT_POINTER(macsec->secy.tx_sc.sa[i], NULL);
			clear_tx_sa(sa);
		}
	}

	free_percpu(macsec->stats);
	free_percpu(macsec->secy.tx_sc.stats);
}

static void macsec_dellink(struct net_device *dev, struct list_head *head)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct net_device *real_dev = macsec->real_dev;
	struct macsec_rxh_data *rxd = macsec_data_rtnl(real_dev);

	unregister_netdevice_queue(dev, head);
	list_del_rcu(&macsec->secys);
	macsec_del_dev(dev);

	if (list_empty(&rxd->secys))
		netdev_rx_handler_unregister(real_dev);

	dev_put(real_dev);
}

static int register_macsec_dev(struct net_device *real_dev,
			       struct net_device *dev)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct macsec_rxh_data *rxd = macsec_data_rtnl(real_dev);

	if (!rxd) {
		int err;

		rxd = kmalloc(sizeof(*rxd), GFP_KERNEL);
		if (!rxd)
			return -ENOMEM;

		INIT_LIST_HEAD(&rxd->secys);

		err = netdev_rx_handler_register(real_dev, macsec_handle_frame,
						 rxd);
		if (err < 0)
			return err;
	}

	list_add_tail_rcu(&macsec->secys, &rxd->secys);
	return 0;
}

static bool sci_exists(struct net_device *dev, sci_t sci)
{
	struct macsec_rxh_data *rxd = macsec_data_rtnl(dev);
	struct macsec_dev *macsec;

	list_for_each_entry(macsec, &rxd->secys, secys) {
		if (macsec->secy.sci == sci)
			return true;
	}

	return false;
}

static sci_t dev_to_sci(struct net_device *dev, __be16 port)
{
	return make_sci(dev->dev_addr, port);
}

static int macsec_add_dev(struct net_device *dev, sci_t sci, u8 icv_len)
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct macsec_secy *secy = &macsec->secy;

	macsec->stats = netdev_alloc_pcpu_stats(struct pcpu_secy_stats);
	if (!macsec->stats)
		return -ENOMEM;

	secy->tx_sc.stats = netdev_alloc_pcpu_stats(struct pcpu_tx_sc_stats);
	if (!secy->tx_sc.stats) {
		free_percpu(macsec->stats);
		return -ENOMEM;
	}

	if (sci == MACSEC_UNDEF_SCI)
		sci = dev_to_sci(dev, MACSEC_PORT_ES);

	secy->netdev = dev;
	secy->operational = true;
	secy->key_len = DEFAULT_SAK_LEN;
	secy->icv_len = icv_len;
	secy->validate_frames = MACSEC_VALIDATE_DEFAULT;
	secy->protect_frames = true;
	secy->replay_protect = false;

	secy->sci = sci;
	secy->tx_sc.active = true;
	secy->tx_sc.encoding_sa = DEFAULT_ENCODING_SA;
	secy->tx_sc.encrypt = DEFAULT_ENCRYPT;
	secy->tx_sc.send_sci = DEFAULT_SEND_SCI;
	secy->tx_sc.end_station = false;
	secy->tx_sc.scb = false;

	return 0;
}

static int macsec_newlink(struct net *net, struct net_device *dev,
			  struct nlattr *tb[], struct nlattr *data[])
{
	struct macsec_dev *macsec = macsec_priv(dev);
	struct net_device *real_dev;
	int err;
	sci_t sci;
	u8 icv_len = DEFAULT_ICV_LEN;
	rx_handler_func_t *rx_handler;

	if (!tb[IFLA_LINK])
		return -EINVAL;
	real_dev = __dev_get_by_index(net, nla_get_u32(tb[IFLA_LINK]));
	if (!real_dev)
		return -ENODEV;

	dev->priv_flags |= IFF_MACSEC;

	macsec->real_dev = real_dev;

	if (data && data[IFLA_MACSEC_ICV_LEN])
		icv_len = nla_get_u8(data[IFLA_MACSEC_ICV_LEN]);
	dev->mtu = real_dev->mtu - icv_len - macsec_extra_len(true);

	rx_handler = rtnl_dereference(real_dev->rx_handler);
	if (rx_handler && rx_handler != macsec_handle_frame)
		return -EBUSY;

	err = register_netdevice(dev);
	if (err < 0)
		return err;

	/* need to be already registered so that ->init has run and
	 * the MAC addr is set
	 */
	if (data && data[IFLA_MACSEC_SCI])
		sci = nla_get_sci(data[IFLA_MACSEC_SCI]);
	else if (data && data[IFLA_MACSEC_PORT])
		sci = dev_to_sci(dev, nla_get_be16(data[IFLA_MACSEC_PORT]));
	else
		sci = dev_to_sci(dev, MACSEC_PORT_ES);

	if (rx_handler && sci_exists(real_dev, sci)) {
		err = -EBUSY;
		goto unregister;
	}

	err = macsec_add_dev(dev, sci, icv_len);
	if (err)
		goto unregister;

	if (data)
		macsec_changelink_common(dev, data);

	err = register_macsec_dev(real_dev, dev);
	if (err < 0)
		goto del_dev;

	dev_hold(real_dev);

	return 0;

del_dev:
	macsec_del_dev(dev);
unregister:
	unregister_netdevice(dev);
	return err;
}

static int macsec_validate_attr(struct nlattr *tb[], struct nlattr *data[])
{
	u64 csid = DEFAULT_CIPHER_ID;
	u8 icv_len = DEFAULT_ICV_LEN;
	int flag;
	bool es, scb, sci;

	if (!data)
		return 0;

	if (data[IFLA_MACSEC_CIPHER_SUITE])
		csid = nla_get_u64(data[IFLA_MACSEC_CIPHER_SUITE]);

	if (data[IFLA_MACSEC_ICV_LEN])
		icv_len = nla_get_u8(data[IFLA_MACSEC_ICV_LEN]);

	switch (csid) {
	case DEFAULT_CIPHER_ID:
	case DEFAULT_CIPHER_ALT:
		if (icv_len < MACSEC_MIN_ICV_LEN ||
		    icv_len > MACSEC_MAX_ICV_LEN)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	if (data[IFLA_MACSEC_ENCODING_SA]) {
		if (nla_get_u8(data[IFLA_MACSEC_ENCODING_SA]) > 3)
			return -EINVAL;
	}

	for (flag = IFLA_MACSEC_ENCODING_SA;
	     flag < IFLA_MACSEC_VALIDATION;
	     flag++) {
		if (data[flag]) {
			if (nla_get_u8(data[flag]) > 1)
				return -EINVAL;
		}
	}

	es  = data[IFLA_MACSEC_ES] ? nla_get_u8(data[IFLA_MACSEC_ES]) : false;
	sci = data[IFLA_MACSEC_INC_SCI] ? nla_get_u8(data[IFLA_MACSEC_INC_SCI]) : false;
	scb = data[IFLA_MACSEC_SCB] ? nla_get_u8(data[IFLA_MACSEC_SCB]) : false;

	if ((sci && (scb || es)) || (scb && es))
		return -EINVAL;

	if (data[IFLA_MACSEC_VALIDATION] &&
	    nla_get_u8(data[IFLA_MACSEC_VALIDATION]) > MACSEC_VALIDATE_MAX)
		return -EINVAL;

	if ((data[IFLA_MACSEC_PROTECT] &&
	     nla_get_u8(data[IFLA_MACSEC_PROTECT])) &&
	    !data[IFLA_MACSEC_WINDOW])
		return -EINVAL;

	return 0;
}

static struct net *macsec_get_link_net(const struct net_device *dev)
{
	return dev_net(macsec_priv(dev)->real_dev);
}

static size_t macsec_get_size(const struct net_device *dev)
{
	return 0 +
		nla_total_size(8) + /* SCI */
		nla_total_size(1) + /* ICV_LEN */
		nla_total_size(8) + /* CIPHER_SUITE */
		nla_total_size(4) + /* WINDOW */
		nla_total_size(1) + /* ENCODING_SA */
		nla_total_size(1) + /* ENCRYPT */
		nla_total_size(1) + /* PROTECT */
		nla_total_size(1) + /* INC_SCI */
		nla_total_size(1) + /* ES */
		nla_total_size(1) + /* SCB */
		nla_total_size(1) + /* REPLAY_PROTECT */
		nla_total_size(1) + /* VALIDATION */
		0;
}

static int macsec_fill_info(struct sk_buff *skb,
			    const struct net_device *dev)
{
	struct macsec_secy *secy = &macsec_priv(dev)->secy;
	struct macsec_tx_sc *tx_sc = &secy->tx_sc;

	if (nla_put_sci(skb, IFLA_MACSEC_SCI, secy->sci) ||
	    nla_put_u8(skb, IFLA_MACSEC_ICV_LEN, secy->icv_len) ||
	    nla_put_u64(skb, IFLA_MACSEC_CIPHER_SUITE, DEFAULT_CIPHER_ID) ||
	    nla_put_u8(skb, IFLA_MACSEC_ENCODING_SA, tx_sc->encoding_sa) ||
	    nla_put_u8(skb, IFLA_MACSEC_ENCRYPT, tx_sc->encrypt) ||
	    nla_put_u8(skb, IFLA_MACSEC_PROTECT, secy->protect_frames) ||
	    nla_put_u8(skb, IFLA_MACSEC_INC_SCI, tx_sc->send_sci) ||
	    nla_put_u8(skb, IFLA_MACSEC_ES, tx_sc->end_station) ||
	    nla_put_u8(skb, IFLA_MACSEC_SCB, tx_sc->scb) ||
	    nla_put_u8(skb, IFLA_MACSEC_REPLAY_PROTECT, secy->replay_protect) ||
	    nla_put_u8(skb, IFLA_MACSEC_VALIDATION, secy->validate_frames) ||
	    0)
		goto nla_put_failure;

	if (secy->replay_protect) {
		if (nla_put_u32(skb, IFLA_MACSEC_WINDOW, secy->replay_window))
			goto nla_put_failure;
	}

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static struct rtnl_link_ops macsec_link_ops __read_mostly = {
	.kind		= "macsec",
	.priv_size	= sizeof(struct macsec_dev),
	.maxtype	= IFLA_MACSEC_MAX,
	.policy		= macsec_rtnl_policy,
	.setup		= macsec_setup,
	.validate	= macsec_validate_attr,
	.newlink	= macsec_newlink,
	.changelink	= macsec_changelink,
	.dellink	= macsec_dellink,
	.get_size	= macsec_get_size,
	.fill_info	= macsec_fill_info,
	.get_link_net	= macsec_get_link_net,
};

static bool is_macsec_master(struct net_device *dev)
{
	bool ret;

	rcu_read_lock();
	ret = rcu_access_pointer(dev->rx_handler) == macsec_handle_frame;
	rcu_read_unlock();

	return ret;
}

static int macsec_notify(struct notifier_block *this, unsigned long event,
			 void *ptr)
{
	struct net_device *real_dev = netdev_notifier_info_to_dev(ptr);
	LIST_HEAD(head);

	if (!is_macsec_master(real_dev))
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UNREGISTER: {
		struct macsec_dev *m, *n;
		struct macsec_rxh_data *rxd;

		rxd = macsec_data_rtnl(real_dev);
		list_for_each_entry_safe(m, n, &rxd->secys, secys) {
			macsec_dellink(m->secy.netdev, &head);
		}
		unregister_netdevice_many(&head);
		break;
	}
	case NETDEV_CHANGEMTU: {
		struct macsec_dev *m;
		struct macsec_rxh_data *rxd;

		rxd = macsec_data_rtnl(real_dev);
		list_for_each_entry(m, &rxd->secys, secys) {
			struct net_device *dev = m->secy.netdev;
			unsigned int mtu = real_dev->mtu - (m->secy.icv_len +
							    macsec_extra_len(true));

			if (dev->mtu > mtu)
				dev_set_mtu(dev, mtu);
		}
	}
	}

	return NOTIFY_OK;
}

static struct notifier_block macsec_notifier = {
	.notifier_call = macsec_notify,
};

static int __init macsec_init(void)
{
	int err;

	pr_info("MACsec IEEE 802.1AE\n");
	err = register_netdevice_notifier(&macsec_notifier);
	if (err)
		return err;

	err = rtnl_link_register(&macsec_link_ops);
	if (err)
		goto notifier;

	err = genl_register_family_with_ops(&macsec_fam, macsec_genl_ops);
	if (err)
		goto rtnl;

	return 0;

rtnl:
	rtnl_link_unregister(&macsec_link_ops);
notifier:
	unregister_netdevice_notifier(&macsec_notifier);
	return err;
}

static void __exit macsec_exit(void)
{
	genl_unregister_family(&macsec_fam);
	rtnl_link_unregister(&macsec_link_ops);
	unregister_netdevice_notifier(&macsec_notifier);
}

module_init(macsec_init);
module_exit(macsec_exit);

MODULE_ALIAS_RTNL_LINK("macsec");

MODULE_DESCRIPTION("MACsec IEEE 802.1AE");
MODULE_LICENSE("GPL v2");
