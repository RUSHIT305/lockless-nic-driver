/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LOCKLESS_NIC_H
#define _LOCKLESS_NIC_H

#include <linux/atomic.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/seqlock.h>
#include <linux/types.h>

#define LNIC_DRV_NAME "lnic"
#define LNIC_DRV_VERSION "0.1.0"
#define LNIC_DEFAULT_MTU 1500
#define LNIC_MIN_MTU 68
#define LNIC_MAX_MTU 9000
#define LNIC_DEFAULT_ORDER 12
#define LNIC_MAX_ORDER 20

/* One cache line per producer/consumer cursor reduces false sharing. */
struct lnic_ring {
	struct sk_buff __rcu **slots;
	u32 mask;
	u32 capacity;
	aligned_u64 head ____cacheline_aligned_in_smp;
	aligned_u64 tail ____cacheline_aligned_in_smp;
};

struct lnic_config {
	struct rcu_head rcu;
	unsigned int queue_count;
	unsigned int ring_order;
	bool loopback;
};

struct lnic_stats {
	u64 tx_packets;
	u64 tx_bytes;
	u64 tx_dropped;
	u64 rx_packets;
	u64 rx_bytes;
	u64 rx_dropped;
	u64 ring_full;
	u64 napi_polls;
	u64 napi_budget_exhausted;
};

struct lnic_priv {
	struct net_device *netdev;
	struct napi_struct napi;
	struct lnic_ring tx_ring;
	struct lnic_ring rx_ring;
	struct lnic_config __rcu *config;
	struct u64_stats_sync stats_sync;
	struct lnic_stats stats;
	atomic_t running;
	unsigned int ring_order;
};

int lnic_ring_init(struct lnic_ring *ring, unsigned int order);
void lnic_ring_destroy(struct lnic_ring *ring);
bool lnic_ring_enqueue(struct lnic_ring *ring, struct sk_buff *skb);
struct sk_buff *lnic_ring_dequeue(struct lnic_ring *ring);
unsigned int lnic_ring_count(const struct lnic_ring *ring);

void lnic_stats_tx_drop(struct lnic_priv *priv, bool full);
void lnic_stats_rx(struct lnic_priv *priv, unsigned int bytes);
void lnic_stats_rx_drop(struct lnic_priv *priv);

#endif /* _LOCKLESS_NIC_H */
