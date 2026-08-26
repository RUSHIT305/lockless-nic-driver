// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/slab.h>

#include "lockless_nic.h"

/*
 * The ring is deliberately SPSC: ndo_start_xmit is the producer and the
 * NAPI poller is the consumer. No spinlock is needed because each cursor has
 * exactly one writer. The release/acquire pairs publish packet ownership.
 */
int lnic_ring_init(struct lnic_ring *ring, unsigned int order)
{
	if (!ring || order < 1 || order > LNIC_MAX_ORDER)
		return -EINVAL;

	ring->capacity = 1U << order;
	ring->mask = ring->capacity - 1;
	ring->slots = kcalloc(ring->capacity, sizeof(*ring->slots), GFP_KERNEL);
	if (!ring->slots)
		return -ENOMEM;

	WRITE_ONCE(ring->head, 0);
	WRITE_ONCE(ring->tail, 0);
	return 0;
}

void lnic_ring_destroy(struct lnic_ring *ring)
{
	unsigned int i;

	if (!ring || !ring->slots)
		return;

	for (i = 0; i < ring->capacity; i++) {
		struct sk_buff *skb;

		skb = rcu_dereference_protected(ring->slots[i], 1);
		if (skb)
			kfree_skb(skb);
	}

	kfree(ring->slots);
	ring->slots = NULL;
}

void lnic_ring_purge(struct lnic_ring *ring)
{
	struct sk_buff *skb;

	while ((skb = lnic_ring_dequeue(ring)))
		kfree_skb(skb);
}

bool lnic_ring_enqueue(struct lnic_ring *ring, struct sk_buff *skb)
{
	u64 head, tail;
	struct sk_buff __rcu **slot;

	if (unlikely(!ring || !ring->slots || !skb))
		return false;

	head = READ_ONCE(ring->head);
	tail = smp_load_acquire(&ring->tail);
	if (unlikely(head - tail >= ring->capacity))
		return false;

	slot = &ring->slots[head & ring->mask];
	/* The consumer clears this slot before advancing tail. */
	if (unlikely(rcu_access_pointer(*slot) != NULL))
		return false;

	rcu_assign_pointer(*slot, skb);
	/* Publish the skb only after its slot is fully initialized. */
	smp_store_release(&ring->head, head + 1);
	return true;
}

struct sk_buff *lnic_ring_dequeue(struct lnic_ring *ring)
{
	u64 head, tail;
	struct sk_buff __rcu **slot;
	struct sk_buff *skb;

	if (unlikely(!ring || !ring->slots))
		return NULL;

	tail = READ_ONCE(ring->tail);
	head = smp_load_acquire(&ring->head);
	if (unlikely(tail == head))
		return NULL;

	slot = &ring->slots[tail & ring->mask];
	rcu_read_lock();
	skb = rcu_dereference(*slot);
	RCU_INIT_POINTER(*slot, NULL);
	rcu_read_unlock();

	/* Release the slot after clearing it, then let the producer reuse it.
	 */
	smp_store_release(&ring->tail, tail + 1);
	return skb;
}

unsigned int lnic_ring_count(const struct lnic_ring *ring)
{
	u64 head, tail;

	if (unlikely(!ring))
		return 0;

	head = smp_load_acquire(&ring->head);
	tail = READ_ONCE(ring->tail);
	return min_t(u64, head - tail, ring->capacity);
}
