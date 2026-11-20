/*
 * Copyright (c) 2024-2026 Antmicro <www.antmicro.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include "virtio_common.h"

LOG_MODULE_REGISTER(virtio, CONFIG_VIRTIO_LOG_LEVEL);

/*
 * Based on Virtual I/O Device (VIRTIO) Version 1.3 specification:
 * https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.pdf
 */

/*
 * The maximum queue size is 2^15 (see 2.7),
 * so any 16bit value larger than that can be used as a sentinel in the next field
 */
#define VIRTQ_DESC_NEXT_SENTINEL 0xffff

/* According to the spec 2.7.5.2 the maximum size of descriptor chain is 4GB */
#define MAX_DESCRIPTOR_CHAIN_LENGTH BIT64(32)

#ifdef CONFIG_VIRTIO_RESTRICTED_DMA_POOL
#define POOL_BLOCK_SIZE CONFIG_VIRTIO_RESTRICTED_DMA_POOL_BLOCK_SIZE

BUILD_ASSERT(IS_POWER_OF_TWO(POOL_BLOCK_SIZE) && POOL_BLOCK_SIZE >= 16,
	     "VIRTIO_RESTRICTED_DMA_POOL_BLOCK_SIZE must be a power of two of at least 16 bytes");

/*
 * Pool memory is handed to the device by its own address (see virtq_dev_addr()),
 * so the pool has to be identity mapped. That is the case without an MMU, and
 * on AArch64, the only architecture that maps zephyr,memory-region nodes, which
 * it maps flat when they carry a zephyr,memory-attr property.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_MMU) || IS_ENABLED(CONFIG_ARM64),
	     "restricted DMA pools need an identity mapping, which with an MMU only AArch64 "
	     "provides");

/* the pool is carved out of memory by hand: it must not overlap the kernel's RAM */
#define VIRTQ_POOL_OVERLAPS_SRAM(node_id)                                                          \
	(DT_REG_ADDR_U64(node_id) <                                                                \
		 (uint64_t)CONFIG_SRAM_BASE_ADDRESS + KB((uint64_t)CONFIG_SRAM_SIZE) &&            \
	 (uint64_t)CONFIG_SRAM_BASE_ADDRESS <                                                      \
		 DT_REG_ADDR_U64(node_id) + (uint64_t)DT_REG_SIZE(node_id))

/*
 * One pool per restricted-dma-pool devicetree node. The region must be a
 * zephyr,memory-region too, which keeps the linker from placing anything else
 * in it (and, with a zephyr,memory-attr property, maps it on AArch64).
 */
#define VIRTQ_POOL_DT_DEFINE(node_id)                                                              \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(node_id, zephyr_memory_region),                            \
		     "restricted-dma-pool nodes must also be zephyr,memory-region nodes");         \
	BUILD_ASSERT(!IS_ENABLED(CONFIG_ARM64) || DT_NODE_HAS_PROP(node_id, zephyr_memory_attr),   \
		     "restricted-dma-pool nodes need a zephyr,memory-attr property to be mapped "  \
		     "on AArch64");                                                                \
	BUILD_ASSERT(!VIRTQ_POOL_OVERLAPS_SRAM(node_id),                                           \
		     "restricted-dma-pool nodes must not overlap the kernel's RAM");               \
	BUILD_ASSERT(DT_REG_ADDR_U64(node_id) % 16 == 0,                                           \
		     "restricted-dma-pool nodes must be 16 bytes aligned, like a descriptor "      \
		     "table");                                                                     \
	BUILD_ASSERT(DT_REG_SIZE(node_id) >= POOL_BLOCK_SIZE,                                      \
		     "restricted-dma-pool nodes must hold at least one block");                    \
	SYS_MEM_BLOCKS_DEFINE_STATIC_WITH_EXT_BUF(UTIL_CAT(VIRTQ_POOL_DT_NAME(node_id), _blocks),  \
						  POOL_BLOCK_SIZE,                                 \
						  DT_REG_SIZE(node_id) / POOL_BLOCK_SIZE,          \
						  (uint8_t *)(uintptr_t)DT_REG_ADDR_U64(node_id)); \
	struct virtq_pool VIRTQ_POOL_DT_NAME(node_id) = {                                          \
		.blocks = &UTIL_CAT(VIRTQ_POOL_DT_NAME(node_id), _blocks),                         \
	};

DT_FOREACH_STATUS_OKAY(restricted_dma_pool, VIRTQ_POOL_DT_DEFINE)

static size_t virtq_pool_block_count(size_t size)
{
	return size / POOL_BLOCK_SIZE + (size % POOL_BLOCK_SIZE != 0 ? 1 : 0);
}

/*
 * Allocates size bytes from the pool. Room in the pool is never waited for:
 * -EBUSY when there is none at the moment, -ENOMEM when the pool could not
 * hold size bytes even if it were empty.
 */
static int virtq_pool_alloc(struct virtq_pool *pool, size_t size, void **block)
{
	size_t count = virtq_pool_block_count(size);

	if (count == 0) {
		return -EINVAL;
	}

	if (count > pool->blocks->info.num_blocks) {
		/* more than the pool will ever hold */
		return -ENOMEM;
	}

	return sys_mem_blocks_alloc_contiguous(pool->blocks, count, block) == 0 ? 0 : -EBUSY;
}

static void virtq_pool_free(struct virtq_pool *pool, void *ptr, size_t size)
{
	int ret = sys_mem_blocks_free_contiguous(pool->blocks, ptr, virtq_pool_block_count(size));

	/* an error here means the pool's bookkeeping is corrupted */
	__ASSERT_NO_MSG(ret == 0);
	ARG_UNUSED(ret);
}

/*
 * Makes a buffer accessible to the device, returning the address to put in the
 * descriptor. Without a pool the device accesses the buffer directly. With a
 * pool the buffer is bounced: a copy in the pool is handed to the device
 * instead, filled now for device-readable buffers and copied back once the
 * device returns the chain for device-writable ones.
 */
static int virtq_dma_map_buf(struct virtq *v, uint16_t desc_idx, const struct virtq_buf *buf,
			     bool device_writable, uintptr_t *dev_addr)
{
	struct virtq_desc_info *info;
	void *bounce;
	int ret;

	if (v->pool == NULL) {
		*dev_addr = k_mem_phys_addr(buf->addr);
		return 0;
	}

	info = &v->desc_info[desc_idx];
	info->bounce = NULL;

	if (buf->len == 0) {
		/* nothing for the device to access, so not even an address to show it */
		*dev_addr = 0;
		return 0;
	}

	ret = virtq_pool_alloc(v->pool, buf->len, &bounce);
	if (ret != 0) {
		LOG_DBG("no room in the pool for %u bytes (%d)", buf->len, ret);
		return ret;
	}

	if (!device_writable) {
		memcpy(bounce, buf->addr, buf->len);
	}

	info->addr = buf->addr;
	info->len = buf->len;
	info->device_writable = device_writable;
	info->bounce = bounce;
	*dev_addr = virtq_dev_addr(v, bounce);

	return 0;
}

/* undoes virtq_dma_map_buf(), see virtio_common.h */
void virtq_dma_unmap_buf(struct virtq *v, uint16_t desc_idx, uint32_t *used_len)
{
	struct virtq_desc_info *info;

	if (v->pool == NULL) {
		return;
	}

	info = &v->desc_info[desc_idx];
	if (info->bounce == NULL) {
		return;
	}

	if (info->device_writable && used_len != NULL) {
		uint32_t len = MIN(info->len, *used_len);

		/* the device's writes to the pool must be visible before they are copied back */
		barrier_dmem_fence_full();
		memcpy(info->addr, info->bounce, len);
		*used_len -= len;
	}

	virtq_pool_free(v->pool, info->bounce, info->len);
	info->bounce = NULL;
}
#else
/* without pools every virtqueue is created with pool == NULL and nothing is bounced */
static int virtq_pool_alloc(struct virtq_pool *pool, size_t size, void **block)
{
	ARG_UNUSED(pool);
	ARG_UNUSED(size);
	ARG_UNUSED(block);
	return -ENOMEM;
}

static void virtq_pool_free(struct virtq_pool *pool, void *ptr, size_t size)
{
	ARG_UNUSED(pool);
	ARG_UNUSED(ptr);
	ARG_UNUSED(size);
}

static int virtq_dma_map_buf(struct virtq *v, uint16_t desc_idx, const struct virtq_buf *buf,
			     bool device_writable, uintptr_t *dev_addr)
{
	ARG_UNUSED(v);
	ARG_UNUSED(desc_idx);
	ARG_UNUSED(device_writable);
	*dev_addr = k_mem_phys_addr(buf->addr);
	return 0;
}

#endif /* CONFIG_VIRTIO_RESTRICTED_DMA_POOL */

int virtq_create_in_pool(struct virtq *v, size_t size, struct virtq_pool *pool)
{
	/*
	 * A size of 0 denotes a queue the driver enumerates but does not use
	 * (e.g. the virtiofs high priority queue). Such a queue is set up empty
	 * and is never operated on. Other split virtqueue sizes must be powers
	 * of two no greater than 32 KiB.
	 */
	if (size > KB(32) || (size != 0U && !IS_POWER_OF_TWO(size))) {
		LOG_ERR("invalid virtqueue size %zu", size);
		return -EINVAL;
	}
	/*
	 * For sizes and alignments see table in spec 2.7. We are supporting only modern virtio, so
	 * we don't have to adhere to additional constraints from spec 2.7.2
	 */
	size_t descriptor_table_size = 16 * size;
	size_t available_ring_size = 2 * size + 6;
	size_t used_ring_pad = (descriptor_table_size + available_ring_size) % 4;
	size_t used_ring_size = 8 * size + 6;
	/*
	 * The descriptor table and the rings are how the driver and the device talk to each
	 * other, so they are the only part of the virtqueue the device has to access.
	 */
	size_t shared_size =
		descriptor_table_size + available_ring_size + used_ring_pad + used_ring_size;
	/*
	 * Everything else (callbacks, the free descriptor stack and, with a pool, the
	 * information needed to restore bounced buffers) is private to the driver. It starts
	 * with the pointer-sized entries, so that each part is aligned for its contents.
	 */
	size_t recv_cbs_size = size * sizeof(struct virtq_receive_callback_entry);
	size_t stack_size = size * sizeof(stack_data_t);
	size_t desc_info_size = pool != NULL ? size * sizeof(struct virtq_desc_info) : 0;
	size_t private_size = recv_cbs_size + stack_size + desc_info_size;
	uint8_t *shared_area;
	uint8_t *private_area;

	if (pool == NULL) {
		/* the device may access everything: one allocation, private part WB_UP()-aligned */
		size_t private_pad = WB_UP(shared_size) - shared_size;

		shared_area = k_aligned_alloc(16, shared_size + private_pad + private_size);
		if (shared_area == NULL) {
			LOG_ERR("unable to allocate virtqueue");
			return -ENOMEM;
		}
		private_area = shared_area + shared_size + private_pad;
	} else {
		/* the rings in the pool shared with the device, the driver's state out of reach */
		void *block;

		if (virtq_pool_alloc(pool, shared_size, &block) != 0) {
			LOG_ERR("unable to allocate virtqueue rings in the pool");
			return -ENOMEM;
		}
		shared_area = block;
		private_area = NULL;
		if (private_size > 0) {
			private_area = k_malloc(private_size);
			if (private_area == NULL) {
				LOG_ERR("unable to allocate virtqueue state");
				virtq_pool_free(pool, shared_area, shared_size);
				return -ENOMEM;
			}
		}
	}

	memset(v, 0, sizeof(*v));
	v->num = size;
	v->pool = pool;
	v->shared_size = shared_size;
	v->desc = (struct virtq_desc *)shared_area;
	v->avail = (struct virtq_avail *)((uint8_t *)v->desc + descriptor_table_size);
	v->used = (struct virtq_used *)((uint8_t *)v->avail + available_ring_size + used_ring_pad);
	v->recv_cbs = (struct virtq_receive_callback_entry *)private_area;
	if (desc_info_size > 0) {
		v->desc_info =
			(struct virtq_desc_info *)(private_area + recv_cbs_size + stack_size);
	}

	/*
	 * At the beginning of the descriptor table, the available ring and the used ring have to be
	 * set to zero. It's the case for both PCI (4.1.5.1.3) and MMIO (4.2.3.2) transport options.
	 * Its unspecified for channel I/O (chapter 4.3), but its used on platforms not supported by
	 * Zephyr, so we don't have to handle it here
	 */
	memset(shared_area, 0, shared_size);
	if (private_area != NULL) {
		memset(private_area, 0, private_size);
	}

	stack_data_t *stack_buf = (stack_data_t *)(private_area + recv_cbs_size);

	k_stack_init(&v->free_desc_stack, stack_buf, size);
	for (uint16_t i = 0; i < size; i++) {
		k_stack_push(&v->free_desc_stack, i);
	}
	v->free_desc_n = size;

	LOG_DBG("virtqueue of %zu descriptors: rings at %p (%zu bytes, %s), state at %p "
		"(%zu bytes)",
		size, (void *)shared_area, shared_size,
		pool != NULL ? "restricted DMA pool" : "heap", (void *)private_area, private_size);

	return 0;
}

int virtq_create(struct virtq *v, size_t size)
{
	return virtq_create_in_pool(v, size, NULL);
}

void virtq_free(struct virtq *v)
{
	if (v->pool == NULL) {
		k_free(v->desc);
	} else {
		virtq_pool_free(v->pool, v->desc, v->shared_size);
		k_free(v->recv_cbs);
	}
}

static void virtq_add_available(struct virtq *v, uint16_t desc_idx)
{
	uint16_t idx = sys_le16_to_cpu(v->avail->idx);

	v->avail->ring[idx & (v->num - 1)] = sys_cpu_to_le16(desc_idx);
	barrier_dmem_fence_full();
	v->avail->idx = sys_cpu_to_le16(idx + 1);
}

/* returns to the free stack, and to the pool, a chain that was not made available */
static void virtq_return_desc_chain(struct virtq *v, uint16_t head, uint16_t count)
{
	uint16_t curr = head;

	for (uint16_t i = 0; i < count; i++) {
		uint16_t next = sys_le16_to_cpu(v->desc[curr].next);

		virtq_dma_unmap_buf(v, curr, NULL);
		virtq_add_free_desc(v, curr);
		curr = next;
	}
}

int virtq_add_buffer_chain(
	struct virtq *v, struct virtq_buf *bufs, uint16_t bufs_size,
	uint16_t device_readable_count, virtq_receive_callback cb, void *cb_opaque,
	k_timeout_t timeout)
{
	if (bufs_size == 0) {
		return -EINVAL;
	}

	uint64_t total_len = 0;

	for (int i = 0; i < bufs_size; i++) {
		total_len += bufs[i].len;
	}

	if (total_len > MAX_DESCRIPTOR_CHAIN_LENGTH) {
		LOG_ERR("buffer chain is longer than 2^32 bytes");
		return -EINVAL;
	}

	uint16_t prev_desc = VIRTQ_DESC_NEXT_SENTINEL;
	uint16_t head = VIRTQ_DESC_NEXT_SENTINEL;

	for (uint16_t buf_n = 0; buf_n < bufs_size; buf_n++) {
		uint16_t desc;
		/* popped outside the queue lock as k_stack_pop() may block */
		int ret = virtq_get_free_desc(v, &desc, timeout);

		if (ret != 0) {
			virtq_return_desc_chain(v, head, buf_n);
			return ret;
		}

		bool device_writable = buf_n >= device_readable_count;
		uintptr_t dev_addr;

		/* room in the pool is not waited for, whatever the timeout */
		ret = virtq_dma_map_buf(v, desc, &bufs[buf_n], device_writable, &dev_addr);
		if (ret != 0) {
			/* not linked to the chain yet */
			virtq_add_free_desc(v, desc);
			virtq_return_desc_chain(v, head, buf_n);
			return ret;
		}

		if (head == VIRTQ_DESC_NEXT_SENTINEL) {
			head = desc;
		}

		uint16_t flags = device_writable ? VIRTQ_DESC_F_WRITE : 0;

		if (buf_n < bufs_size - 1) {
			flags |= VIRTQ_DESC_F_NEXT;
		} else {
			v->desc[desc].next = 0;
		}
		v->desc[desc].addr = sys_cpu_to_le64(dev_addr);
		v->desc[desc].len = sys_cpu_to_le32(bufs[buf_n].len);
		v->desc[desc].flags = sys_cpu_to_le16(flags);

		if (prev_desc != VIRTQ_DESC_NEXT_SENTINEL) {
			v->desc[prev_desc].next = sys_cpu_to_le16(desc);
		}

		prev_desc = desc;
	}

	v->recv_cbs[head].cb = cb;
	v->recv_cbs[head].opaque = cb_opaque;

	k_spinlock_key_t key = k_spin_lock(&v->lock);

	virtq_add_available(v, head);
	k_spin_unlock(&v->lock, key);

	return 0;
}

int virtq_get_free_desc(struct virtq *v, uint16_t *desc_idx, k_timeout_t timeout)
{
	stack_data_t desc;

	int ret = k_stack_pop(&v->free_desc_stack, &desc, timeout);

	if (ret == 0) {
		*desc_idx = (uint16_t)desc;
		K_SPINLOCK(&v->lock) {
			v->free_desc_n--;
		}
	}

	return ret;
}

void virtq_add_free_desc(struct virtq *v, uint16_t desc_idx)
{
	k_stack_push(&v->free_desc_stack, desc_idx);
	K_SPINLOCK(&v->lock) {
		v->free_desc_n++;
	}
}
