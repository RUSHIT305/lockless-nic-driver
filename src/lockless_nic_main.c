// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>

#include "lockless_nic.h"

static unsigned int ring_order = LNIC_DEFAULT_ORDER;
module_param(ring_order, uint, 0444);
MODULE_PARM_DESC(ring_order, "Log2 of each lock-free ring capacity (1-20)");

static bool loopback = true;
module_param(loopback, bool, 0444);
MODULE_PARM_DESC(loopback, "Mirror transmitted packets into the RX ring");

static struct net_device *lnic_dev;

static void lnic_stats_tx(struct lnic_priv *priv, unsigned int bytes)
{
	unsigned long flags;

	flags = u64_stats_update_begin_irqsave(&priv->stats_sync);
	priv->stats.tx_packets++;
	priv->stats.tx_bytes += bytes;
	u64_stats_update_end_irqrestore(&priv->stats_sync, flags);
}

void lnic_stats_tx_drop(struct lnic_priv *priv, bool full)
{
	unsigned long flags;

	flags = u64_stats_update_begin_irqsave(&priv->stats_sync);
	priv->stats.tx_dropped++;
	if (full)
		priv->stats.ring_full++;
	u64_stats_update_end_irqrestore(&priv->stats_sync, flags);
}

void lnic_stats_ring_full(struct lnic_priv *priv)
{
	unsigned long flags;

	flags = u64_stats_update_begin_irqsave(&priv->stats_sync);
	priv->stats.ring_full++;
	u64_stats_update_end_irqrestore(&priv->stats_sync, flags);
}

void lnic_stats_rx(struct lnic_priv *priv, unsigned int bytes)
{
	unsigned long flags;

	flags = u64_stats_update_begin_irqsave(&priv->stats_sync);
	priv->stats.rx_packets++;
	priv->stats.rx_bytes += bytes;
	u64_stats_update_end_irqrestore(&priv->stats_sync, flags);
}

void lnic_stats_rx_drop(struct lnic_priv *priv)
{
	unsigned long flags;

	flags = u64_stats_update_begin_irqsave(&priv->stats_sync);
	priv->stats.rx_dropped++;
	u64_stats_update_end_irqrestore(&priv->stats_sync, flags);
}

static void lnic_stats_poll(struct lnic_priv *priv, bool exhausted)
{
	unsigned long flags;

	flags = u64_stats_update_begin_irqsave(&priv->stats_sync);
	priv->stats.napi_polls++;
	if (exhausted)
		priv->stats.napi_budget_exhausted++;
	u64_stats_update_end_irqrestore(&priv->stats_sync, flags);
}

static netdev_tx_t lnic_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct lnic_priv *priv = netdev_priv(dev);
	unsigned int len = skb->len;

	if (unlikely(!atomic_read(&priv->running))) {
		lnic_stats_tx_drop(priv, false);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (unlikely(!lnic_ring_enqueue(&priv->tx_ring, skb))) {
		lnic_stats_ring_full(priv);
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}

	lnic_stats_tx(priv, len);
	if (napi_schedule_prep(&priv->napi))
		__napi_schedule(&priv->napi);
	return NETDEV_TX_OK;
}

static void lnic_process_tx(struct lnic_priv *priv, bool mirror)
{
	struct sk_buff *skb;
	bool drained = false;

	while ((skb = lnic_ring_dequeue(&priv->tx_ring))) {
		drained = true;
		if (mirror) {
			if (unlikely(!lnic_ring_enqueue(&priv->rx_ring, skb))) {
				lnic_stats_rx_drop(priv);
				dev_kfree_skb_any(skb);
			}
		} else {
			dev_kfree_skb_any(skb);
		}
	}
	if (drained)
		netif_wake_queue(priv->netdev);
}

static int lnic_poll(struct napi_struct *napi, int budget)
{
	struct lnic_priv *priv = container_of(napi, struct lnic_priv, napi);
	struct lnic_config *cfg;
	struct sk_buff *skb;
	bool mirror = false;
	int work_done = 0;

	/* Copy the read-mostly policy, then leave RCU before packet processing.
	 */
	rcu_read_lock();
	cfg = rcu_dereference(priv->config);
	mirror = cfg && READ_ONCE(cfg->loopback);
	rcu_read_unlock();
	lnic_process_tx(priv, mirror);

	while (work_done < budget) {
		skb = lnic_ring_dequeue(&priv->rx_ring);
		if (!skb)
			break;

		skb->protocol = eth_type_trans(skb, priv->netdev);
		skb_reset_network_header(skb);
		lnic_stats_rx(priv, skb->len);
		napi_gro_receive(napi, skb);
		work_done++;
	}

	lnic_stats_poll(priv, work_done == budget);
	if (work_done < budget) {
		napi_complete_done(napi, work_done);
		if ((lnic_ring_count(&priv->tx_ring) ||
		     lnic_ring_count(&priv->rx_ring)) &&
		    napi_schedule_prep(napi))
			__napi_schedule(napi);
	}

	return work_done;
}

static int lnic_open(struct net_device *dev)
{
	struct lnic_priv *priv = netdev_priv(dev);

	atomic_set(&priv->running, 1);
	napi_enable(&priv->napi);
	netif_start_queue(dev);
	return 0;
}

static int lnic_stop(struct net_device *dev)
{
	struct lnic_priv *priv = netdev_priv(dev);

	netif_stop_queue(dev);
	atomic_set(&priv->running, 0);
	napi_disable(&priv->napi);
	lnic_ring_purge(&priv->tx_ring);
	lnic_ring_purge(&priv->rx_ring);
	return 0;
}

static int lnic_change_mtu(struct net_device *dev, int new_mtu)
{
	if (new_mtu < LNIC_MIN_MTU || new_mtu > LNIC_MAX_MTU)
		return -EINVAL;

	WRITE_ONCE(dev->mtu, new_mtu);
	return 0;
}

static void lnic_get_stats64(struct net_device *dev,
			     struct rtnl_link_stats64 *s)
{
	struct lnic_priv *priv = netdev_priv(dev);
	unsigned int start;

	do {
		start = u64_stats_fetch_begin(&priv->stats_sync);
		s->tx_packets = priv->stats.tx_packets;
		s->tx_bytes = priv->stats.tx_bytes;
		s->tx_dropped = priv->stats.tx_dropped;
		s->rx_packets = priv->stats.rx_packets;
		s->rx_bytes = priv->stats.rx_bytes;
		s->rx_dropped = priv->stats.rx_dropped;
	} while (u64_stats_fetch_retry(&priv->stats_sync, start));
}

static const struct net_device_ops lnic_netdev_ops = {
    .ndo_open = lnic_open,
    .ndo_stop = lnic_stop,
    .ndo_start_xmit = lnic_start_xmit,
    .ndo_change_mtu = lnic_change_mtu,
    .ndo_get_stats64 = lnic_get_stats64,
};

static void lnic_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &lnic_netdev_ops;
	dev->needs_free_netdev = true;
	dev->min_mtu = LNIC_MIN_MTU;
	dev->max_mtu = LNIC_MAX_MTU;
	dev->mtu = LNIC_DEFAULT_MTU;
	dev->tx_queue_len = 256;
	dev->features |= NETIF_F_SG | NETIF_F_HW_CSUM;
	eth_hw_addr_random(dev);
}

static int lnic_init_priv(struct lnic_priv *priv)
{
	struct lnic_config *cfg;
	int err;

	priv->ring_order = ring_order;
	if (ring_order < 1 || ring_order > LNIC_MAX_ORDER)
		return -EINVAL;

	err = lnic_ring_init(&priv->tx_ring, ring_order);
	if (err)
		return err;

	err = lnic_ring_init(&priv->rx_ring, ring_order);
	if (err) {
		lnic_ring_destroy(&priv->tx_ring);
		return err;
	}

	cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg) {
		lnic_ring_destroy(&priv->rx_ring);
		lnic_ring_destroy(&priv->tx_ring);
		return -ENOMEM;
	}
	cfg->queue_count = 1;
	cfg->ring_order = ring_order;
	cfg->loopback = loopback;
	RCU_INIT_POINTER(priv->config, cfg);
	u64_stats_init(&priv->stats_sync);
	atomic_set(&priv->running, 0);
	return 0;
}

static void lnic_cleanup_priv(struct lnic_priv *priv)
{
	struct lnic_config *cfg;

	cfg = rcu_replace_pointer(priv->config, NULL, true);
	synchronize_rcu();
	kfree(cfg);
	lnic_ring_destroy(&priv->rx_ring);
	lnic_ring_destroy(&priv->tx_ring);
}

static int __init lnic_init(void)
{
	struct lnic_priv *priv;
	int err;

	lnic_dev =
	    alloc_netdev(sizeof(*priv), "lnic%d", NET_NAME_UNKNOWN, lnic_setup);
	if (!lnic_dev)
		return -ENOMEM;

	priv = netdev_priv(lnic_dev);
	priv->netdev = lnic_dev;
	netif_napi_add(lnic_dev, &priv->napi, lnic_poll);

	err = lnic_init_priv(priv);
	if (err)
		goto err_napi_del;

	err = register_netdev(lnic_dev);
	if (err)
		goto err_priv;

	pr_info("registered %s (%s), ring capacity=%u, loopback=%s\n",
		lnic_dev->name,
		LNIC_DRV_VERSION,
		priv->rx_ring.capacity,
		loopback ? "on" : "off");
	return 0;

err_priv:
	lnic_cleanup_priv(priv);
err_napi_del:
	netif_napi_del(&priv->napi);
	free_netdev(lnic_dev);
	lnic_dev = NULL;
	return err;
}

static void __exit lnic_exit(void)
{
	struct lnic_priv *priv;

	if (!lnic_dev)
		return;

	priv = netdev_priv(lnic_dev);
	/* unregister_netdev() invokes ndo_stop when the interface is up. */
	unregister_netdev(lnic_dev);
	netif_napi_del(&priv->napi);
	lnic_cleanup_priv(priv);
	free_netdev(lnic_dev);
	lnic_dev = NULL;
	pr_info("unregistered\n");
}

module_init(lnic_init);
module_exit(lnic_exit);

MODULE_AUTHOR("Manus AI");
MODULE_DESCRIPTION("Lock-less virtual NIC using RCU, SPSC rings, and NAPI");
MODULE_LICENSE("GPL");
MODULE_VERSION(LNIC_DRV_VERSION);
