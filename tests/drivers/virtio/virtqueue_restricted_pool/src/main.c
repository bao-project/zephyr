/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A virtqueue created in a restricted DMA pool must keep everything the device
 * accesses inside the pool: the rings are allocated from it and the buffers
 * added to the queue are bounced through it. The pool is the region the board
 * overlay declares as test_pool. The tests play the device: they write to the
 * memory the descriptors point to, add used ring entries and run the interrupt
 * handler the transports share.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/virtio.h>
#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/mem_blocks.h>
#include <zephyr/ztest.h>

#include "virtio_common.h"

#define POOL_NODE  DT_NODELABEL(test_pool)
#define POOL_BASE  DT_REG_ADDR(POOL_NODE)
#define POOL_SIZE  DT_REG_SIZE(POOL_NODE)
#define QUEUE_SIZE 8

/* sizes of the parts of a split virtqueue of QUEUE_SIZE descriptors, spec 2.7 */
#define DESC_TABLE_SIZE (16 * QUEUE_SIZE)
#define AVAIL_RING_SIZE (6 + 2 * QUEUE_SIZE)
#define USED_RING_SIZE  (6 + 8 * QUEUE_SIZE)

struct virtqueue_restricted_pool_fixture {
	struct virtq vq;
};

/* a buffer followed by a canary, to catch copies running past its end */
struct guarded_buf {
	uint8_t data[32];
	uint8_t canary[16];
};

static struct guarded_buf rx;
static uint8_t tx[32];
/*
 * Big enough that three of them fill the pool next to the rings: a quarter of
 * the pool each, and the rings of the fixture's queue, which stay allocated for
 * the whole suite, take a few blocks, so a fourth never fits.
 */
static uint8_t big[POOL_SIZE / 4];

static bool in_pool(uintptr_t addr, size_t len)
{
	return addr >= POOL_BASE && addr + len <= POOL_BASE + POOL_SIZE;
}

static uint16_t last_posted_head(const struct virtq *vq)
{
	uint16_t idx = sys_le16_to_cpu(vq->avail->idx) - 1;

	return sys_le16_to_cpu(vq->avail->ring[idx % QUEUE_SIZE]);
}

/* the address the device was given for a descriptor, as a pointer */
static uint8_t *dev_ptr(const struct virtq *vq, uint16_t desc)
{
	return (uint8_t *)(uintptr_t)sys_le64_to_cpu(vq->desc[desc].addr);
}

/* the virtqueue the transport stand-in hands to virtio_isr() */
static struct virtq *isr_vq;

static struct virtq *isr_get_virtqueue(const struct device *dev, uint16_t queue_idx)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(queue_idx);

	return isr_vq;
}

static DEVICE_API(virtio, transport_api) = {.get_virtqueue = isr_get_virtqueue};
static const struct device transport = {.api = &transport_api};

/* what the callback of the last returned chain was told */
static uint32_t callback_used_len;
static unsigned int callback_count;

static void chain_callback(void *opaque, uint32_t used_len)
{
	ARG_UNUSED(opaque);

	callback_used_len = used_len;
	callback_count++;
}

/*
 * The device returns a chain: it adds an entry to the used ring and raises the
 * used buffer notification. Returns the used length the callback was told.
 */
static uint32_t device_returns(struct virtq *vq, uint16_t head, uint32_t used_len)
{
	uint16_t used_idx = sys_le16_to_cpu(vq->used->idx);
	unsigned int count = callback_count;

	vq->used->ring[used_idx % QUEUE_SIZE].id = sys_cpu_to_le32(head);
	vq->used->ring[used_idx % QUEUE_SIZE].len = sys_cpu_to_le32(used_len);
	vq->used->idx = sys_cpu_to_le16(used_idx + 1);

	isr_vq = vq;
	virtio_isr(&transport, VIRTIO_QUEUE_INTERRUPT, 1);

	zassert_equal(callback_count, count + 1, "the callback of the chain did not run once");

	return callback_used_len;
}

/* adds one buffer of len bytes of buf as its own chain */
static int add_one(struct virtq *vq, void *buf, uint32_t len, bool device_writable,
		   k_timeout_t timeout)
{
	struct virtq_buf vbuf = {.addr = buf, .len = len};

	return virtq_add_buffer_chain(vq, &vbuf, 1, device_writable ? 0 : 1, chain_callback, NULL,
				      timeout);
}

/* fills the pool with chains of big buffers until the next one does not fit */
static int fill_pool(struct virtq *vq, uint16_t *heads, int max)
{
	int added = 0;

	while (added < max && add_one(vq, big, sizeof(big), false, K_NO_WAIT) == 0) {
		heads[added++] = last_posted_head(vq);
	}

	return added;
}

static void *setup(void)
{
	static struct virtqueue_restricted_pool_fixture fixture;

	zassert_ok(virtq_create_in_pool(&fixture.vq, QUEUE_SIZE, VIRTQ_POOL_DT_GET(POOL_NODE)));
	return &fixture;
}

static void teardown(void *fixture)
{
	struct virtqueue_restricted_pool_fixture *f = fixture;

	virtq_free(&f->vq);
}

ZTEST_F(virtqueue_restricted_pool, test_pool_covers_the_devicetree_region)
{
	struct virtq_pool *pool = VIRTQ_POOL_DT_GET(POOL_NODE);

	zassert_equal((uintptr_t)pool->blocks->buffer, POOL_BASE);
	zassert_equal(pool->blocks->info.num_blocks << pool->blocks->info.blk_sz_shift, POOL_SIZE);
	zassert_equal(fixture->vq.pool, pool);
}

ZTEST_F(virtqueue_restricted_pool, test_rings_in_pool_and_driver_state_outside)
{
	struct virtq *vq = &fixture->vq;

	zassert_true(in_pool((uintptr_t)vq->desc, DESC_TABLE_SIZE));
	zassert_true(in_pool((uintptr_t)vq->avail, AVAIL_RING_SIZE));
	zassert_true(in_pool((uintptr_t)vq->used, USED_RING_SIZE));

	zassert_false(in_pool((uintptr_t)vq->recv_cbs, 1));
	zassert_false(in_pool((uintptr_t)vq->desc_info, 1));
	zassert_false(in_pool((uintptr_t)vq->free_desc_stack.base, 1));
}

ZTEST(virtqueue_restricted_pool, test_queue_without_pool_is_unchanged)
{
	struct virtq plain;

	zassert_ok(virtq_create(&plain, QUEUE_SIZE));
	zassert_is_null(plain.pool);
	zassert_false(in_pool((uintptr_t)plain.desc, 1));

	zassert_ok(add_one(&plain, tx, sizeof(tx), false, K_NO_WAIT));
	uint16_t head = last_posted_head(&plain);

	/* the device gets the driver's buffer itself */
	zassert_equal(sys_le64_to_cpu(plain.desc[head].addr), k_mem_phys_addr(tx));

	device_returns(&plain, head, 0);
	virtq_free(&plain);
}

ZTEST(virtqueue_restricted_pool, test_free_returns_the_rings_to_the_pool)
{
	struct virtq_pool *pool = VIRTQ_POOL_DT_GET(POOL_NODE);
	size_t block_size = BIT(pool->blocks->info.blk_sz_shift);
	struct virtq second;

	zassert_ok(virtq_create_in_pool(&second, QUEUE_SIZE, pool));
	void *rings = second.desc;
	size_t blocks = DIV_ROUND_UP(second.shared_size, block_size);

	zassert_false(sys_mem_blocks_is_region_free(pool->blocks, rings, blocks));
	virtq_free(&second);
	zassert_true(sys_mem_blocks_is_region_free(pool->blocks, rings, blocks));
}

ZTEST_F(virtqueue_restricted_pool, test_device_readable_buffer_is_copied_into_pool)
{
	struct virtq *vq = &fixture->vq;

	for (size_t i = 0; i < sizeof(tx); i++) {
		tx[i] = (uint8_t)(0x40 + i);
	}

	zassert_ok(add_one(vq, tx, sizeof(tx), false, K_NO_WAIT));
	uint16_t head = last_posted_head(vq);
	uint8_t *bounce = dev_ptr(vq, head);

	/* the descriptor points into the pool, at a copy of the buffer */
	zassert_true(in_pool((uintptr_t)bounce, sizeof(tx)));
	zassert_not_equal(bounce, tx);
	zassert_equal(sys_le32_to_cpu(vq->desc[head].len), sizeof(tx));
	zassert_mem_equal(bounce, tx, sizeof(tx));
	zassert_equal(vq->desc_info[head].bounce, bounce);

	device_returns(vq, head, 0);
	zassert_is_null(vq->desc_info[head].bounce);
	zassert_equal(vq->free_desc_n, QUEUE_SIZE);
}

ZTEST_F(virtqueue_restricted_pool, test_device_writable_buffer_is_copied_back_within_bounds)
{
	struct virtq *vq = &fixture->vq;
	struct virtq_buf bufs[] = {
		{.addr = tx, .len = sizeof(tx)},
		{.addr = rx.data, .len = sizeof(rx.data)},
	};
	uint8_t pattern[sizeof(rx.data)];

	memset(&rx, 0xAA, sizeof(rx));
	for (size_t i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)i;
	}

	zassert_ok(virtq_add_buffer_chain(vq, bufs, 2, 1, chain_callback, NULL, K_NO_WAIT));
	uint16_t head = last_posted_head(vq);
	uint16_t second = sys_le16_to_cpu(vq->desc[head].next);
	uint8_t *bounce = dev_ptr(vq, second);

	zassert_true(in_pool((uintptr_t)bounce, sizeof(rx.data)));
	zassert_equal(sys_le16_to_cpu(vq->desc[second].flags), VIRTQ_DESC_F_WRITE);
	/* the driver's buffer is untouched until the device returns the chain */
	zassert_equal(rx.data[0], 0xAA);

	/* the device fills the whole bounce buffer but reports fewer bytes used */
	memcpy(bounce, pattern, sizeof(pattern));
	/* the callback is told what the device reported, not what is left after the copy */
	zassert_equal(device_returns(vq, head, 20), 20);

	zassert_mem_equal(rx.data, pattern, 20);
	for (size_t i = 20; i < sizeof(rx.data); i++) {
		zassert_equal(rx.data[i], 0xAA, "byte %zu copied beyond used length", i);
	}
	for (size_t i = 0; i < sizeof(rx.canary); i++) {
		zassert_equal(rx.canary[i], 0xAA, "canary byte %zu overwritten", i);
	}
}

ZTEST_F(virtqueue_restricted_pool, test_used_length_is_spread_over_writable_buffers)
{
	struct virtq *vq = &fixture->vq;
	struct guarded_buf second_rx;
	struct virtq_buf bufs[] = {
		{.addr = rx.data, .len = sizeof(rx.data)},
		{.addr = second_rx.data, .len = sizeof(second_rx.data)},
	};

	memset(&rx, 0xAA, sizeof(rx));
	memset(&second_rx, 0xAA, sizeof(second_rx));

	zassert_ok(virtq_add_buffer_chain(vq, bufs, 2, 0, chain_callback, NULL, K_NO_WAIT));
	uint16_t head = last_posted_head(vq);
	uint16_t second = sys_le16_to_cpu(vq->desc[head].next);

	/* the device fills both buffers but reports the first one and 8 bytes of the second */
	memset(dev_ptr(vq, head), 0x11, sizeof(rx.data));
	memset(dev_ptr(vq, second), 0x22, sizeof(second_rx.data));

	zassert_equal(device_returns(vq, head, sizeof(rx.data) + 8), sizeof(rx.data) + 8);

	for (size_t i = 0; i < sizeof(rx.data); i++) {
		zassert_equal(rx.data[i], 0x11, "first buffer byte %zu not copied", i);
	}
	for (size_t i = 0; i < 8; i++) {
		zassert_equal(second_rx.data[i], 0x22, "second buffer byte %zu not copied", i);
	}
	for (size_t i = 8; i < sizeof(second_rx.data); i++) {
		zassert_equal(second_rx.data[i], 0xAA, "byte %zu copied beyond used length", i);
	}
	zassert_equal(rx.canary[0], 0xAA);
	zassert_equal(second_rx.canary[0], 0xAA);
}

ZTEST_F(virtqueue_restricted_pool, test_used_length_beyond_buffer_does_not_overrun_it)
{
	struct virtq *vq = &fixture->vq;

	memset(&rx, 0xAA, sizeof(rx));
	zassert_ok(add_one(vq, rx.data, sizeof(rx.data), true, K_NO_WAIT));
	uint16_t head = last_posted_head(vq);

	memset(dev_ptr(vq, head), 0x55, sizeof(rx.data));
	/* a used length larger than the buffer must not make the copy run past it */
	device_returns(vq, head, UINT32_MAX);

	for (size_t i = 0; i < sizeof(rx.data); i++) {
		zassert_equal(rx.data[i], 0x55);
	}
	for (size_t i = 0; i < sizeof(rx.canary); i++) {
		zassert_equal(rx.canary[i], 0xAA, "canary byte %zu overwritten", i);
	}
}

ZTEST_F(virtqueue_restricted_pool, test_zero_length_buffer_is_not_bounced)
{
	struct virtq *vq = &fixture->vq;

	zassert_ok(add_one(vq, tx, 0, false, K_NO_WAIT));
	uint16_t head = last_posted_head(vq);

	/* nothing to copy, so the device is not even shown an address */
	zassert_is_null(vq->desc_info[head].bounce);
	zassert_equal(sys_le64_to_cpu(vq->desc[head].addr), 0);
	zassert_equal(sys_le32_to_cpu(vq->desc[head].len), 0);

	device_returns(vq, head, 0);
	zassert_equal(vq->free_desc_n, QUEUE_SIZE);
}

ZTEST_F(virtqueue_restricted_pool, test_pool_exhaustion_fails_and_recovers)
{
	struct virtq *vq = &fixture->vq;
	uint16_t heads[4];

	/* 4 buffers of a quarter of the pool cannot fit next to the rings */
	int added = fill_pool(vq, heads, 4);

	zassert_equal(added, 3, "expected the pool to hold 3 buffers, held %d", added);
	zassert_equal(add_one(vq, big, sizeof(big), false, K_NO_WAIT), -EBUSY);
	/* the failed chain did not leak its descriptor */
	zassert_equal(vq->free_desc_n, QUEUE_SIZE - 3);

	/* returning one chain makes room again */
	device_returns(vq, heads[0], 0);
	zassert_ok(add_one(vq, big, sizeof(big), false, K_NO_WAIT));
	heads[0] = last_posted_head(vq);

	for (int i = 0; i < 3; i++) {
		device_returns(vq, heads[i], 0);
	}
	zassert_equal(vq->free_desc_n, QUEUE_SIZE);
}

ZTEST_F(virtqueue_restricted_pool, test_exhaustion_on_a_later_buffer_frees_the_whole_chain)
{
	struct virtq *vq = &fixture->vq;
	struct virtq_buf bufs[] = {
		{.addr = big, .len = sizeof(big)},
		{.addr = big, .len = sizeof(big)},
		{.addr = big, .len = sizeof(big)},
		{.addr = big, .len = sizeof(big)},
	};
	uint16_t heads[3];

	/* the fourth buffer does not fit: the first three are unmapped again */
	zassert_equal(virtq_add_buffer_chain(vq, bufs, 4, 4, chain_callback, NULL, K_NO_WAIT),
		      -EBUSY);
	zassert_equal(vq->free_desc_n, QUEUE_SIZE);
	zassert_equal(fill_pool(vq, heads, 3), 3, "pool blocks of the failed chain leaked");

	for (int i = 0; i < 3; i++) {
		device_returns(vq, heads[i], 0);
	}
}

ZTEST_F(virtqueue_restricted_pool, test_request_larger_than_pool_fails_at_once)
{
	static uint8_t huge[POOL_SIZE + 1];
	struct virtq *vq = &fixture->vq;
	int64_t start = k_uptime_ticks();

	/* it can never fit, which is told apart from a pool that is full at the moment */
	zassert_equal(add_one(vq, huge, sizeof(huge), false, K_FOREVER), -ENOMEM);
	zassert_true(k_uptime_ticks() - start < k_ms_to_ticks_ceil32(100), "waited for room");
	zassert_equal(vq->free_desc_n, QUEUE_SIZE);
}

ZTEST_F(virtqueue_restricted_pool, test_full_pool_is_not_waited_for)
{
	struct virtq *vq = &fixture->vq;
	uint16_t heads[3];

	zassert_equal(fill_pool(vq, heads, 3), 3);

	int64_t start = k_uptime_ticks();

	/* the timeout only applies to free descriptors, of which there are plenty */
	zassert_equal(add_one(vq, big, sizeof(big), false, K_MSEC(200)), -EBUSY);
	zassert_equal(add_one(vq, big, sizeof(big), false, K_FOREVER), -EBUSY);
	zassert_true(k_uptime_ticks() - start < k_ms_to_ticks_ceil32(100), "waited for room");
	zassert_equal(vq->free_desc_n, QUEUE_SIZE - 3);

	for (int i = 0; i < 3; i++) {
		device_returns(vq, heads[i], 0);
	}
	zassert_equal(vq->free_desc_n, QUEUE_SIZE);
}

ZTEST(virtqueue_restricted_pool, test_queue_too_big_for_pool_is_rejected)
{
	struct virtq huge;

	/* 32 Ki descriptors need 512 KiB of descriptor table alone */
	zassert_equal(virtq_create_in_pool(&huge, KB(32), VIRTQ_POOL_DT_GET(POOL_NODE)), -ENOMEM);
}

ZTEST_SUITE(virtqueue_restricted_pool, NULL, setup, NULL, NULL, teardown);
