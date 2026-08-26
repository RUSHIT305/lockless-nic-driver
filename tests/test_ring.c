// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CAPACITY 1024u
#define ITEMS    1000000u

struct packet {
	uint64_t sequence;
	uint64_t checksum;
};

struct ring {
	struct packet *slots[CAPACITY];
	_Atomic uint64_t head;
	_Atomic uint64_t tail;
};

struct context {
	struct ring ring;
	_Atomic bool failed;
};

static bool enqueue(struct ring *ring, struct packet *packet)
{
	uint64_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
	uint64_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

	if (head - tail >= CAPACITY)
		return false;
	assert(ring->slots[head & (CAPACITY - 1)] == NULL);
	ring->slots[head & (CAPACITY - 1)] = packet;
	atomic_store_explicit(&ring->head, head + 1, memory_order_release);
	return true;
}

static struct packet *dequeue(struct ring *ring)
{
	uint64_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
	uint64_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
	struct packet *packet;

	if (tail == head)
		return NULL;
	packet = ring->slots[tail & (CAPACITY - 1)];
	ring->slots[tail & (CAPACITY - 1)] = NULL;
	atomic_store_explicit(&ring->tail, tail + 1, memory_order_release);
	return packet;
}

static void *producer(void *arg)
{
	struct context *ctx = arg;
	uint64_t next = 0;

	while (next < ITEMS) {
		struct packet *packet = malloc(sizeof(*packet));
		if (!packet) {
			atomic_store(&ctx->failed, true);
			return NULL;
		}
		packet->sequence = next;
		packet->checksum = next ^ UINT64_C(0x9e3779b97f4a7c15);
		if (enqueue(&ctx->ring, packet))
			next++;
		else
			free(packet);
	}
	return NULL;
}

static void *consumer(void *arg)
{
	struct context *ctx = arg;
	uint64_t expected = 0;

	while (expected < ITEMS) {
		struct packet *packet = dequeue(&ctx->ring);
		if (!packet)
			continue;
		if (packet->sequence != expected ||
		    packet->checksum != (expected ^ UINT64_C(0x9e3779b97f4a7c15)))
			atomic_store(&ctx->failed, true);
		free(packet);
		expected++;
	}
	return NULL;
}

int main(void)
{
	struct context ctx = { 0 };
	pthread_t tx, rx;

	assert(pthread_create(&tx, NULL, producer, &ctx) == 0);
	assert(pthread_create(&rx, NULL, consumer, &ctx) == 0);
	assert(pthread_join(tx, NULL) == 0);
	assert(pthread_join(rx, NULL) == 0);
	assert(!atomic_load(&ctx.failed));
	assert(atomic_load(&ctx.ring.head) == ITEMS);
	assert(atomic_load(&ctx.ring.tail) == ITEMS);
	puts("SPSC ring stress test passed");
	return 0;
}
