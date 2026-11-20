/*
 * Copyright (c) 2025 Antmicro <www.antmicro.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_VIRTIO_VIRTIO_COMMON_H_
#define ZEPHYR_VIRTIO_VIRTIO_COMMON_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/mem_blocks.h>
#include <zephyr/drivers/virtio/virtio_config.h>
#include <zephyr/drivers/virtio/virtqueue.h>

/*
 * Driver-private information about a descriptor, kept where the device cannot
 * reach it.
 */
struct virtq_desc_info {
	/*
	 * For a virtqueue in a restricted DMA pool, the descriptor shows the
	 * device a copy of the driver's buffer. These fields tell where data is
	 * copied back to, and how much.
	 */
	/* buffer passed by the driver */
	void *addr;
	/* copy of the buffer in the pool, NULL when the buffer is not bounced */
	void *bounce;
	/* length of the buffer */
	uint32_t len;
	/* the buffer is written by the device */
	bool device_writable;
};

/*
 * Memory pool shared with a virtio device: a block allocator over the memory
 * the device is allowed to access. The allocator keeps its bookkeeping (the
 * bitmap) outside the pool, so the device cannot corrupt it. One pool exists
 * per restricted-dma-pool devicetree node and is shared by all the devices
 * referencing it.
 */
struct virtq_pool {
	sys_mem_blocks_t *blocks;
};

/* name of the pool of a restricted-dma-pool devicetree node */
#define VIRTQ_POOL_DT_NAME(node_id) UTIL_CAT(virtq_pool_, DT_DEP_ORD(node_id))

/* pointer to the pool of a restricted-dma-pool devicetree node */
#define VIRTQ_POOL_DT_GET(node_id) (&VIRTQ_POOL_DT_NAME(node_id))

/* pointer to the pool a device references through its memory-region property */
#define VIRTQ_POOL_DT_GET_BY_MEMORY_REGION(node_id)                                                \
	VIRTQ_POOL_DT_GET(DT_PHANDLE(node_id, memory_region))

#define VIRTQ_POOL_DT_DECLARE(node_id) extern struct virtq_pool VIRTQ_POOL_DT_NAME(node_id);
DT_FOREACH_STATUS_OKAY(restricted_dma_pool, VIRTQ_POOL_DT_DECLARE)

/**
 * Address the device uses for memory of a virtqueue
 *
 * A restricted DMA pool lies outside the memory the kernel manages and is
 * identity mapped (see the restricted-dma-pool binding), so its addresses are
 * used as they are. Memory of a virtqueue without a pool is in the kernel's
 * RAM mapping.
 *
 * @param v virtqueue the memory belongs to
 * @param ptr pointer to the memory
 * @return physical address to put in a descriptor or program in the transport
 */
static inline uintptr_t virtq_dev_addr(const struct virtq *v, void *ptr)
{
	return v->pool != NULL ? (uintptr_t)ptr : k_mem_phys_addr(ptr);
}

/**
 * Common virtio isr
 *
 * @param dev virtio device it operates on
 * @param isr_status value of isr status register
 * @param virtqueue_count amount of available virtqueues
 */
void virtio_isr(const struct device *dev, uint8_t isr_status, uint16_t virtqueue_count);

#ifdef CONFIG_VIRTIO_RESTRICTED_DMA_POOL
/**
 * Gives a buffer the device returned back to the driver
 *
 * To be called for every descriptor of a returned chain, in the order of the
 * chain, before the descriptor is freed. Nothing is done for a virtqueue
 * without a pool. For a virtqueue in a restricted DMA pool, the data the
 * device wrote to the copy of a device-writable buffer is copied back to the
 * driver's buffer, bounded by the length of the buffer and by @p used_len,
 * and the copy is returned to the pool. The transports call this from their
 * interrupt handler, so the copy runs in interrupt context and takes time
 * proportional to the data the device returned.
 *
 * @param v virtqueue it operates on
 * @param desc_idx index of the descriptor
 * @param used_len amount of bytes written by the device that were not copied
 * back yet, decreased by the amount copied back, so that the used length of a
 * chain is spread over its buffers in order; NULL to discard what the device wrote
 */
void virtq_dma_unmap_buf(struct virtq *v, uint16_t desc_idx, uint32_t *used_len);
#else
static inline void virtq_dma_unmap_buf(struct virtq *v, uint16_t desc_idx, uint32_t *used_len)
{
	ARG_UNUSED(v);
	ARG_UNUSED(desc_idx);
	ARG_UNUSED(used_len);
}
#endif /* CONFIG_VIRTIO_RESTRICTED_DMA_POOL */

#endif /*ZEPHYR_VIRTIO_VIRTIO_COMMON_H_*/
