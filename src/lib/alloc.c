// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

#include <sof/debug/panic.h>
#include <sof/lib/alloc.h>
#include <sof/lib/cache.h>
#include <sof/lib/cpu.h>
#include <sof/lib/dma.h>
#include <sof/lib/memory.h>
#include <sof/lib/mm_heap.h>
#include <sof/spinlock.h>
#include <sof/string.h>
#include <ipc/topology.h>
#include <ipc/trace.h>
#include <config.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define trace_mem_error(__e, ...) \
	trace_error(TRACE_CLASS_MEM, __e, ##__VA_ARGS__)
#define trace_mem_init(__e, ...) \
	trace_event(TRACE_CLASS_MEM, __e, ##__VA_ARGS__)

/* debug to set memory value on every allocation */
#if CONFIG_DEBUG_BLOCK_FREE
#define DEBUG_BLOCK_FREE_VALUE_8BIT ((uint8_t)0xa5)
#define DEBUG_BLOCK_FREE_VALUE_32BIT ((uint32_t)0xa5a5a5a5)
#endif

/* We have 3 memory pools
 *
 * 1) System memory pool does not have a map and it's size is fixed at build
 *    time. Memory cannot be freed from this pool. Used by device drivers
 *    and any system core. Saved as part of PM context.
 * 2) Runtime memory pool has variable size allocation map and memory is freed
 *    on calls to rfree(). Saved as part of PM context. Global size
 *    set at build time.
 * 3) Buffer memory pool has fixed size allocation map and can be freed on
 *    module removal or calls to rfree(). Saved as part of PM context.
 */

#if CONFIG_DEBUG_BLOCK_FREE
/* Check whole memory region for debug pattern to find if memory was freed
 * second time
 */
static void validate_memory(void *ptr, size_t size)
{
	uint32_t *ptr_32 = ptr;
	int i, not_matching = 0;

	for (i = 0; i < size / 4; i++) {
		if (ptr_32[i] != DEBUG_BLOCK_FREE_VALUE_32BIT)
			not_matching = 1;
	}

	if (not_matching) {
		trace_mem_init("validate_memory() pointer:"
			"%p freed pattern not detected",
			(uintptr_t)ptr);
	} else {
		trace_mem_error(
			"validate_memory() freeing pointer:"
			"%p double free detected",
			(uintptr_t)ptr);
	}
}
#endif

/* total size of block */
static inline uint32_t block_get_size(struct block_map *map)
{
	uint32_t size = sizeof(*map) + map->count *
		(map->block_size + sizeof(struct block_hdr));

	platform_shared_commit(map, sizeof(*map));

	return size;
}

/* total size of heap */
static inline uint32_t heap_get_size(struct mm_heap *heap)
{
	uint32_t size = sizeof(struct mm_heap);
	int i;

	for (i = 0; i < heap->blocks; i++)
		size += block_get_size(&heap->map[i]);

	platform_shared_commit(heap, sizeof(*heap));

	return size;
}

#if CONFIG_DEBUG_BLOCK_FREE
static void write_pattern(struct mm_heap *heap_map, int heap_depth,
			  uint8_t pattern)
{
	struct mm_heap *heap;
	struct block_map *current_map;
	int i, j;

	for (i = 0; i < heap_depth; i++) {
		heap = &heap_map[i];

		for (j = 0; j < heap->blocks; j++) {
			current_map = &heap->map[j];
			memset(
				(void *)current_map->base, pattern,
				current_map->count * current_map->block_size);
			platform_shared_commit(current_map,
					       sizeof(*current_map));
		}

		platform_shared_commit(heap, sizeof(*heap));
	}
}
#endif

static void init_heap_map(struct mm_heap *heap, int count)
{
	struct block_map *next_map;
	struct block_map *current_map;
	int i;
	int j;

	for (i = 0; i < count; i++) {
		/* init the map[0] */
		current_map = &heap[i].map[0];
		current_map->base = heap[i].heap;
		platform_shared_commit(current_map, sizeof(*current_map));

		/* map[j]'s base is calculated based on map[j-1] */
		for (j = 1; j < heap[i].blocks; j++) {
			next_map = &heap[i].map[j];
			next_map->base = current_map->base +
				current_map->block_size *
				current_map->count;
			platform_shared_commit(next_map, sizeof(*next_map));
			platform_shared_commit(current_map,
					       sizeof(*current_map));

			current_map = &heap[i].map[j];
		}

		platform_shared_commit(&heap[i], sizeof(heap[i]));
	}
}

/* allocate from system memory pool */
static void *rmalloc_sys(uint32_t flags, int caps, int core, size_t bytes)
{
	struct mm *memmap = memmap_get();
	void *ptr;
	struct mm_heap *cpu_heap;
	size_t alignment = 0;

	/* use the heap dedicated for the selected core */
	cpu_heap = memmap->system + core;
	if ((cpu_heap->caps & caps) != caps)
		panic(SOF_IPC_PANIC_MEM);

	/* align address to dcache line size */
	if (cpu_heap->info.used % PLATFORM_DCACHE_ALIGN)
		alignment = PLATFORM_DCACHE_ALIGN -
			(cpu_heap->info.used % PLATFORM_DCACHE_ALIGN);

	/* always succeeds or panics */
	if (alignment + bytes > cpu_heap->info.free) {
		trace_mem_error("rmalloc_sys(): core = %d, bytes = %d",
				core, bytes);
		panic(SOF_IPC_PANIC_MEM);
	}
	cpu_heap->info.used += alignment;

	ptr = (void *)(cpu_heap->heap + cpu_heap->info.used);

	cpu_heap->info.used += bytes;
	cpu_heap->info.free -= alignment + bytes;

	if (flags & SOF_MEM_FLAG_SHARED)
		ptr = platform_shared_get(ptr, bytes);

	platform_shared_commit(cpu_heap, sizeof(*cpu_heap));
	platform_shared_commit(memmap, sizeof(*memmap));

	return ptr;
}

/* At this point the pointer we have should be unaligned
 * (it was checked level higher) and be power of 2
 */
static void *align_ptr(struct mm_heap *heap, uint32_t alignment,
		       void *ptr, struct block_hdr *hdr)
{
	int mod_align = 0;

	/* Save unaligned ptr to block hdr */
	hdr->unaligned_ptr = ptr;

	/* If ptr is not already aligned we calculate alignment shift */
	if (alignment && (uintptr_t)ptr % alignment)
		mod_align = alignment - ((uintptr_t)ptr % alignment);

	/* Calculate aligned pointer */
	ptr = (char *)ptr + mod_align;

	return ptr;
}

/* allocate single block */
static void *alloc_block(struct mm_heap *heap, int level,
			 uint32_t caps, uint32_t alignment)
{
	struct block_map *map = &heap->map[level];
	struct block_hdr *hdr;
	void *ptr;
	int i;

	hdr = &map->block[map->first_free];

	map->free_count--;
	ptr = (void *)(map->base + map->first_free * map->block_size);
	ptr = align_ptr(heap, alignment, ptr, hdr);

	hdr->size = 1;
	hdr->used = 1;

	heap->info.used += map->block_size;
	heap->info.free -= map->block_size;

	/* find next free */
	for (i = map->first_free; i < map->count; ++i) {
		hdr = &map->block[i];

		if (hdr->used == 0) {
			map->first_free = i;
			break;
		}
	}

	platform_shared_commit(map->block, sizeof(*map->block) * map->count);
	platform_shared_commit(map, sizeof(*map));
	platform_shared_commit(heap, sizeof(*heap));

	return ptr;
}

/* allocates continuous blocks */
static void *alloc_cont_blocks(struct mm_heap *heap, int level,
			       uint32_t caps, size_t bytes, uint32_t alignment)
{
	struct block_map *map = &heap->map[level];
	struct block_hdr *hdr;
	void *ptr = NULL;
	void *unaligned_ptr;
	unsigned int start = map->first_free;
	unsigned int current;
	unsigned int count = bytes / map->block_size;
	unsigned int remaining = 0;

	if (bytes % map->block_size)
		count++;

	/* check if we have enough consecutive blocks for requested
	 * allocation size.
	 */
	for (current = map->first_free; current < map->count &&
	     remaining < count; current++) {
		hdr = &map->block[current];

		if (hdr->used)
			remaining = 0;/* used, not suitable, reset */
		else if (!remaining++)
			start = current;/* new start */
	}

	if (count > map->count || remaining < count) {
		trace_mem_error("%d blocks needed for allocation but only %d blocks are remaining",
				count, remaining);
		goto out;
	}

	/* we found enough space, let's allocate it */
	map->free_count -= count;
	ptr = (void *)(map->base + start * map->block_size);
	unaligned_ptr = ptr;

	hdr = &map->block[start];
	hdr->size = count;

	ptr = align_ptr(heap, alignment, ptr, hdr);

	heap->info.used += count * map->block_size;
	heap->info.free -= count * map->block_size;
	/* update first_free if needed */
	if (map->first_free == start)
		/* find first available free block */
		for (map->first_free += count; map->first_free < map->count;
			map->first_free++) {
			hdr = &map->block[map->first_free];

			if (!hdr->used)
				break;
		}

	/* update each block */
	for (current = start; current < start + count; current++) {
		hdr = &map->block[current];
		hdr->used = 1;
		hdr->unaligned_ptr = unaligned_ptr;
	}

out:
	platform_shared_commit(map->block, sizeof(*map->block) * map->count);
	platform_shared_commit(map, sizeof(*map));
	platform_shared_commit(heap, sizeof(*heap));

	return ptr;
}

static struct mm_heap *get_heap_from_ptr(void *ptr)
{
	struct mm *memmap = memmap_get();
	struct mm_heap *heap;
	int i;

	/* find mm_heap that ptr belongs to */
	heap = memmap->system_runtime + cpu_get_id();
	if ((uint32_t)ptr >= heap->heap &&
	    (uint32_t)ptr < heap->heap + heap->size)
		goto out;

	platform_shared_commit(heap, sizeof(*heap));

	for (i = 0; i < PLATFORM_HEAP_RUNTIME; i++) {
		heap = &memmap->runtime[i];

		if ((uint32_t)ptr >= heap->heap &&
		    (uint32_t)ptr < heap->heap + heap->size)
			goto out;

		platform_shared_commit(heap, sizeof(*heap));
	}

	for (i = 0; i < PLATFORM_HEAP_BUFFER; i++) {
		heap = &memmap->buffer[i];

		if ((uint32_t)ptr >= heap->heap &&
		    (uint32_t)ptr < heap->heap + heap->size)
			goto out;

		platform_shared_commit(heap, sizeof(*heap));
	}

	platform_shared_commit(memmap, sizeof(*memmap));

	return NULL;

out:
	platform_shared_commit(heap, sizeof(*heap));
	platform_shared_commit(memmap, sizeof(*memmap));

	return heap;
}

static struct mm_heap *get_heap_from_caps(struct mm_heap *heap, int count,
					  uint32_t caps)
{
	uint32_t mask;
	int i;

	/* find first heap that support type */
	for (i = 0; i < count; i++) {
		mask = heap[i].caps & caps;
		platform_shared_commit(&heap[i], sizeof(heap[i]));
		if (mask == caps)
			return &heap[i];
	}

	return NULL;
}

static void *get_ptr_from_heap(struct mm_heap *heap, uint32_t flags,
			       uint32_t caps, size_t bytes, uint32_t alignment)
{
	struct block_map *map;
	int i, temp_bytes = bytes;
	void *ptr = NULL;

	/* Only allow alignment as a power of 2 */
	if ((alignment & (alignment - 1)) != 0)
		panic(SOF_IPC_PANIC_MEM);

	for (i = 0; i < heap->blocks; i++) {
		map = &heap->map[i];

		/* size of requested buffer is adjusted for alignment purposes
		 * we check if first free block is already aligned if not
		 * we need to allocate bigger size for alignment
		 */
		if (alignment &&
		    ((map->base + (map->block_size * map->first_free)) %
		     alignment))
			temp_bytes += alignment;

		/* is block big enough */
		if (map->block_size < temp_bytes) {
			platform_shared_commit(map, sizeof(*map));
			temp_bytes = bytes;
			continue;
		}

		/* does block have free space */
		if (map->free_count == 0) {
			platform_shared_commit(map, sizeof(*map));
			temp_bytes = bytes;
			continue;
		}

		/* free block space exists */
		ptr = alloc_block(heap, i, caps, alignment);

		platform_shared_commit(map, sizeof(*map));

		break;
	}

	if (ptr && (flags & SOF_MEM_FLAG_SHARED))
		ptr = platform_shared_get(ptr, bytes);

	return ptr;
}

/* free block(s) */
static void free_block(void *ptr)
{
	struct mm_heap *heap;
	struct block_map *block_map = NULL;
	struct block_hdr *hdr;
	int i;
	int block;
	int used_blocks;
	bool heap_is_full;

	heap = get_heap_from_ptr(ptr);
	if (!heap) {
		trace_mem_error("free_block(): invalid heap = %p, cpu = %d",
				(uintptr_t)ptr, cpu_get_id());
		return;
	}

	/* find block that ptr belongs to */
	for (i = 0; i < heap->blocks; i++) {
		block_map = &heap->map[i];

		/* is ptr in this block */
		if ((uint32_t)ptr < (block_map->base +
		    (block_map->block_size * block_map->count)))
			break;

		platform_shared_commit(block_map, sizeof(*block_map));
	}

	if (i == heap->blocks) {
		platform_shared_commit(heap, sizeof(*heap));

		/* not found */
		trace_mem_error("free_block(): invalid ptr = %p cpu = %d",
				(uintptr_t)ptr, cpu_get_id());
		return;
	}

	/* calculate block header */
	block = ((uint32_t)ptr - block_map->base) / block_map->block_size;

	hdr = &block_map->block[block];

	/* bring back original unaligned pointer position
	 * and calculate correct hdr for free operation (it could
	 * be from different block since we got user pointer here
	 * or null if header was not set)
	 */
	if (hdr->unaligned_ptr != ptr && hdr->unaligned_ptr) {
		ptr = hdr->unaligned_ptr;
		block = ((uint32_t)ptr - block_map->base)
			 / block_map->block_size;
		hdr = &block_map->block[block];
	}

	/* report an error if ptr is not aligned to block */
	if (block_map->base + block_map->block_size * block != (uint32_t)ptr)
		panic(SOF_IPC_PANIC_MEM);

	heap_is_full = !block_map->free_count;

	/* free block header and continuous blocks */
	used_blocks = block + hdr->size;

	for (i = block; i < used_blocks; i++) {
		hdr = &block_map->block[i];
		hdr->size = 0;
		hdr->used = 0;
		hdr->unaligned_ptr = NULL;
		block_map->free_count++;
		heap->info.used -= block_map->block_size;
		heap->info.free += block_map->block_size;
	}

	/* set first free block */
	if (block < block_map->first_free || heap_is_full)
		block_map->first_free = block;

#if CONFIG_DEBUG_BLOCK_FREE
	/* memset the whole block in case of unaligned ptr */
	validate_memory(
		(void *)(block_map->base + block_map->block_size * block),
		block_map->block_size * (i - block));
	memset(
		(void *)(block_map->base + block_map->block_size * block),
		DEBUG_BLOCK_FREE_VALUE_8BIT, block_map->block_size *
		(i - block));
#endif

	platform_shared_commit(block_map->block, sizeof(*block_map->block) *
			       block_map->count);
	platform_shared_commit(block_map, sizeof(*block_map));
	platform_shared_commit(heap, sizeof(*heap));
}

#if CONFIG_DEBUG_HEAP

static void trace_heap_blocks(struct mm_heap *heap)
{
	struct block_map *block_map;
	int i;

	trace_mem_error("heap: 0x%x size %d blocks %d caps 0x%x", heap->heap,
			heap->size, heap->blocks, heap->caps);
	trace_mem_error(" used %d free %d", heap->info.used,
			heap->info.free);

	for (i = 0; i < heap->blocks; i++) {
		block_map = &heap->map[i];

		trace_mem_error(" block %d base 0x%x size %d count %d", i,
				block_map->base, block_map->block_size,
				block_map->count);
		trace_mem_error("  free %d first at %d",
				block_map->free_count, block_map->first_free);

		platform_shared_commit(block_map, sizeof(*block_map));
	}

	platform_shared_commit(heap, sizeof(*heap));
}

static void alloc_trace_heap(enum mem_zone zone, uint32_t caps, size_t bytes)
{
	struct mm *memmap = memmap_get();
	struct mm_heap *heap_base;
	struct mm_heap *heap;
	unsigned int heap_count;
	unsigned int n;
	unsigned int i = 0;
	int count = 0;

	switch (zone) {
	case SOF_MEM_ZONE_RUNTIME:
		heap_base = memmap->runtime;
		heap_count = PLATFORM_HEAP_RUNTIME;
		break;
	case SOF_MEM_ZONE_BUFFER:
		heap_base = memmap->buffer;
		heap_count = PLATFORM_HEAP_BUFFER;
		break;
	default:
		trace_mem_error("alloc trace: unsupported mem zone");
		goto out;
	}
	heap = heap_base;
	n = heap_count;

	while (i < heap_count) {
		heap = get_heap_from_caps(heap, n, caps);
		if (!heap)
			break;

		trace_heap_blocks(heap);
		count++;
		i = heap - heap_base + 1;
		n = heap_count - i;
		heap++;
	}

	if (count == 0)
		trace_mem_error("heap: none found for zone %d caps 0x%x, "
				"bytes 0x%x", zone, caps, bytes);
out:
	platform_shared_commit(memmap, sizeof(*memmap));
	return;
}

#define DEBUG_TRACE_PTR(ptr, bytes, zone, caps, flags) \
	do { \
		if (!ptr) { \
			trace_mem_error("failed to alloc 0x%x bytes zone 0x%x caps 0x%x flags 0x%x", \
					bytes, zone, caps, flags); \
			alloc_trace_heap(zone, caps, bytes); \
		} \
	} while (0)
#else
#define DEBUG_TRACE_PTR(ptr, bytes, zone, caps, flags)
#endif

/* allocate single block for system runtime */
static void *rmalloc_sys_runtime(uint32_t flags, int caps, int core,
				 size_t bytes)
{
	struct mm *memmap = memmap_get();
	struct mm_heap *cpu_heap;
	void *ptr;

	/* use the heap dedicated for the selected core */
	cpu_heap = memmap->system_runtime + core;
	if ((cpu_heap->caps & caps) != caps)
		panic(SOF_IPC_PANIC_MEM);

	ptr = get_ptr_from_heap(cpu_heap, flags, caps, bytes,
				PLATFORM_DCACHE_ALIGN);

	platform_shared_commit(cpu_heap, sizeof(*cpu_heap));
	platform_shared_commit(memmap, sizeof(*memmap));

	return ptr;
}

/* allocate single block for runtime */
static void *rmalloc_runtime(uint32_t flags, uint32_t caps, size_t bytes)
{
	struct mm *memmap = memmap_get();
	struct mm_heap *heap;

	/* check runtime heap for capabilities */
	heap = get_heap_from_caps(memmap->runtime, PLATFORM_HEAP_RUNTIME, caps);
	if (!heap) {
		/* next check buffer heap for capabilities */
		heap = get_heap_from_caps(memmap->buffer, PLATFORM_HEAP_BUFFER,
					  caps);
		if (!heap) {
			platform_shared_commit(memmap, sizeof(*memmap));

			trace_mem_error("rmalloc_runtime(): caps = %x, bytes = %d",
					caps, bytes);

			return NULL;
		}
	}

	platform_shared_commit(memmap, sizeof(*memmap));

	return get_ptr_from_heap(heap, flags, caps, bytes,
				 PLATFORM_DCACHE_ALIGN);
}

static void *_malloc_unlocked(enum mem_zone zone, uint32_t flags, uint32_t caps,
			      size_t bytes)
{
	struct mm *memmap = memmap_get();
	void *ptr = NULL;

	switch (zone) {
	case SOF_MEM_ZONE_SYS:
		ptr = rmalloc_sys(flags, caps, cpu_get_id(), bytes);
		break;
	case SOF_MEM_ZONE_SYS_RUNTIME:
		ptr = rmalloc_sys_runtime(flags, caps, cpu_get_id(), bytes);
		break;
	case SOF_MEM_ZONE_RUNTIME:
		ptr = rmalloc_runtime(flags, caps, bytes);
		break;
	default:
		trace_mem_error("rmalloc(): invalid zone");
		panic(SOF_IPC_PANIC_MEM); /* logic non recoverable problem */
		break;
	}

#if CONFIG_DEBUG_BLOCK_FREE
	if (ptr)
		bzero(ptr, bytes);
#endif

	memmap->heap_trace_updated = 1;

	platform_shared_commit(memmap, sizeof(*memmap));

	return ptr;
}

void *rmalloc(enum mem_zone zone, uint32_t flags, uint32_t caps, size_t bytes)
{
	struct mm *memmap = memmap_get();
	uint32_t lock_flags;
	void *ptr = NULL;

	spin_lock_irq(&memmap->lock, lock_flags);

	ptr = _malloc_unlocked(zone, flags, caps, bytes);

	spin_unlock_irq(&memmap->lock, lock_flags);

	DEBUG_TRACE_PTR(ptr, bytes, zone, caps, flags);
	return ptr;
}

/* allocates and clears memory - not for direct use, clients use rzalloc() */
void *rzalloc(enum mem_zone zone, uint32_t flags, uint32_t caps, size_t bytes)
{
	void *ptr;

	ptr = rmalloc(zone, flags, caps, bytes);
	if (ptr)
		bzero(ptr, bytes);

	DEBUG_TRACE_PTR(ptr, bytes, zone, caps, flags);
	return ptr;
}

void *rzalloc_core_sys(int core, size_t bytes)
{
	struct mm *memmap = memmap_get();
	uint32_t flags;
	void *ptr = NULL;

	spin_lock_irq(&memmap->lock, flags);

	ptr = rmalloc_sys(0, 0, core, bytes);
	if (ptr)
		bzero(ptr, bytes);

	spin_unlock_irq(&memmap->lock, flags);

	return ptr;
}

/* allocates continuous buffers - not for direct use, clients use rballoc() */
static void *alloc_heap_buffer(struct mm_heap *heap, uint32_t flags,
			       uint32_t caps, size_t bytes, uint32_t alignment)
{
	struct block_map *map;
	int i, temp_bytes = bytes;
	void *ptr = NULL;

	/* Only allow alignment as a power of 2 */
	if ((alignment & (alignment - 1)) != 0)
		panic(SOF_IPC_PANIC_MEM);

	/* will request fit in single block */
	for (i = 0; i < heap->blocks; i++) {
		map = &heap->map[i];

		/* size of requested buffer is adjusted for alignment purposes
		 * we check if first free block is already aligned if not
		 * we need to allocate bigger size for alignment
		 */
		if ((map->base + (map->block_size * map->first_free))
		    % alignment)
			temp_bytes += alignment;

		/* Check if blocks are big enough and at least one is free */
		if (map->block_size >= temp_bytes && map->free_count) {
			platform_shared_commit(map, sizeof(*map));

			/* found: grab a block */
			ptr = alloc_block(heap, i, caps, alignment);
			break;
		}
		temp_bytes = bytes;

		platform_shared_commit(map, sizeof(*map));
	}

	/* size of requested buffer is adjusted for alignment purposes
	 * since we span more blocks we have to assume worst case scenario
	 */
	bytes += alignment;

	/* request spans > 1 block */
	if (!ptr) {
		/*
		 * Find the best block size for request. We know, that we failed
		 * to find a single large enough block, so, skip those.
		 */
		for (i = heap->blocks - 1; i >= 0; i--) {
			map = &heap->map[i];

			/* allocate if block size is smaller than request */
			if (heap->size >= bytes	&& map->block_size < bytes) {
				ptr = alloc_cont_blocks(heap, i, caps,
							bytes, alignment);
				if (ptr) {
					platform_shared_commit(map,
							       sizeof(*map));
					break;
				}
			}

			platform_shared_commit(map, sizeof(*map));
		}
	}

	if (ptr && (flags & SOF_MEM_FLAG_SHARED))
		ptr = platform_shared_get(ptr, bytes);

#if CONFIG_DEBUG_BLOCK_FREE
	if (ptr)
		bzero(ptr, temp_bytes);
#endif

	platform_shared_commit(heap, sizeof(*heap));

	return ptr;
}

static void *_balloc_unlocked(uint32_t flags, uint32_t caps, size_t bytes,
			      uint32_t alignment)
{
	struct mm *memmap = memmap_get();
	struct mm_heap *heap;
	unsigned int i, n;
	void *ptr = NULL;

	for (i = 0, n = PLATFORM_HEAP_BUFFER, heap = memmap->buffer;
	     i < PLATFORM_HEAP_BUFFER;
	     i = heap - memmap->buffer + 1, n = PLATFORM_HEAP_BUFFER - i,
	     heap++) {
		heap = get_heap_from_caps(heap, n, caps);
		if (!heap)
			break;

		ptr = alloc_heap_buffer(heap, flags, caps, bytes, alignment);
		if (ptr)
			break;

		/* Continue from the next heap */
	}

	platform_shared_commit(memmap, sizeof(*memmap));

	return ptr;
}

/* allocates continuous buffers - not for direct use, clients use rballoc() */
void *rballoc_align(uint32_t flags, uint32_t caps, size_t bytes,
		    uint32_t alignment)
{
	struct mm *memmap = memmap_get();
	void *ptr = NULL;
	uint32_t lock_flags;

	spin_lock_irq(&memmap->lock, lock_flags);

	ptr = _balloc_unlocked(flags, caps, bytes, alignment);

	spin_unlock_irq(&memmap->lock, lock_flags);

	DEBUG_TRACE_PTR(ptr, bytes, SOF_MEM_ZONE_BUFFER, caps, flags);
	return ptr;
}

static void _rfree_unlocked(void *ptr)
{
	struct mm *memmap = memmap_get();
	struct mm_heap *cpu_heap;

	/* sanity check - NULL ptrs are fine */
	if (!ptr)
		return;

	/* prepare pointer if it's platform requirement */
	ptr = platform_rfree_prepare(ptr);

	/* use the heap dedicated for the selected core */
	cpu_heap = memmap->system + cpu_get_id();

	/* panic if pointer is from system heap */
	if (ptr >= (void *)cpu_heap->heap &&
	    (char *)ptr < (char *)cpu_heap->heap + cpu_heap->size) {
		trace_mem_error("rfree(): attempt to free system heap = %p, cpu = %d",
				(uintptr_t)ptr, cpu_get_id());
		panic(SOF_IPC_PANIC_MEM);
	}

	/* free the block */
	free_block(ptr);
	memmap->heap_trace_updated = 1;

	platform_shared_commit(cpu_heap, sizeof(*cpu_heap));
	platform_shared_commit(memmap, sizeof(*memmap));
}

void rfree(void *ptr)
{
	struct mm *memmap = memmap_get();
	uint32_t flags;

	spin_lock_irq(&memmap->lock, flags);
	_rfree_unlocked(ptr);
	spin_unlock_irq(&memmap->lock, flags);
}

void *rrealloc(void *ptr, enum mem_zone zone, uint32_t flags, uint32_t caps,
	       size_t bytes)
{
	struct mm *memmap = memmap_get();
	void *new_ptr = NULL;
	uint32_t lock_flags;

	if (!bytes)
		return new_ptr;

	spin_lock_irq(&memmap->lock, lock_flags);

	new_ptr = _malloc_unlocked(zone, flags, caps, bytes);

	if (new_ptr && ptr)
		memcpy_s(new_ptr, bytes, ptr, bytes);

	if (new_ptr)
		_rfree_unlocked(ptr);

	spin_unlock_irq(&memmap->lock, lock_flags);

	DEBUG_TRACE_PTR(ptr, bytes, zone, caps, flags);
	return new_ptr;
}

void *rbrealloc_align(void *ptr, uint32_t flags, uint32_t caps, size_t bytes,
		      uint32_t alignment)
{
	struct mm *memmap = memmap_get();
	void *new_ptr = NULL;
	uint32_t lock_flags;

	if (!bytes)
		return new_ptr;

	spin_lock_irq(&memmap->lock, lock_flags);

	new_ptr = _balloc_unlocked(flags, caps, bytes, alignment);

	if (new_ptr && ptr)
		memcpy_s(new_ptr, bytes, ptr, bytes);

	if (new_ptr)
		_rfree_unlocked(ptr);

	spin_unlock_irq(&memmap->lock, lock_flags);

	DEBUG_TRACE_PTR(ptr, bytes, SOF_MEM_ZONE_BUFFER, caps, flags);
	return new_ptr;
}

/* TODO: all mm_pm_...() routines to be implemented for IMR storage */
uint32_t mm_pm_context_size(void)
{
	return 0;
}

/*
 * Save the DSP memories that are in use the system and modules.
 * All pipeline and modules must be disabled before calling this functions.
 * No allocations are permitted after calling this and before calling restore.
 */
int mm_pm_context_save(struct dma_copy *dc, struct dma_sg_config *sg)
{
	return -ENOTSUP;
}

/*
 * Restore the DSP memories to modules and the system.
 * This must be called immediately after booting before any pipeline work.
 */
int mm_pm_context_restore(struct dma_copy *dc, struct dma_sg_config *sg)
{
	return -ENOTSUP;
}

void free_heap(enum mem_zone zone)
{
	struct mm *memmap = memmap_get();
	struct mm_heap *cpu_heap;

	/* to be called by slave cores only for sys heap,
	 * otherwise this is critical flow issue.
	 */
	if (cpu_get_id() == PLATFORM_MASTER_CORE_ID ||
	    zone != SOF_MEM_ZONE_SYS) {
		trace_mem_error("free_heap(): critical flow issue");
		panic(SOF_IPC_PANIC_MEM);
	}

	cpu_heap = memmap->system + cpu_get_id();
	cpu_heap->info.used = 0;
	cpu_heap->info.free = cpu_heap->size;

	platform_shared_commit(cpu_heap, sizeof(*cpu_heap));
	platform_shared_commit(memmap, sizeof(*memmap));
}

#if CONFIG_TRACE
void heap_trace(struct mm_heap *heap, int size)
{
	struct block_map *current_map;
	int i;
	int j;

	for (i = 0; i < size; i++) {
		trace_mem_init(" heap: 0x%x size %d blocks %d caps 0x%x",
			       heap->heap, heap->size, heap->blocks,
			       heap->caps);
		trace_mem_init("  used %d free %d", heap->info.used,
			       heap->info.free);

		/* map[j]'s base is calculated based on map[j-1] */
		for (j = 1; j < heap->blocks; j++) {
			current_map = &heap->map[j];

			trace_mem_init("  block %d base 0x%x size %d",
				       j, current_map->base,
				       current_map->block_size);
			trace_mem_init("   count %d free %d",
				       current_map->count,
				       current_map->free_count);

			platform_shared_commit(current_map,
					       sizeof(*current_map));
		}

		platform_shared_commit(heap, sizeof(*heap));

		heap++;
	}
}

void heap_trace_all(int force)
{
	struct mm *memmap = memmap_get();

	/* has heap changed since last shown */
	if (memmap->heap_trace_updated || force) {
		trace_mem_init("heap: buffer status");
		heap_trace(memmap->buffer, PLATFORM_HEAP_BUFFER);
		trace_mem_init("heap: runtime status");
		heap_trace(memmap->runtime, PLATFORM_HEAP_RUNTIME);
	}

	memmap->heap_trace_updated = 0;

	platform_shared_commit(memmap, sizeof(*memmap));
}
#else
void heap_trace_all(int force) { }
void heap_trace(struct mm_heap *heap, int size) { }
#endif

/* initialise map */
void init_heap(struct sof *sof)
{
	struct mm *memmap = sof->memory_map;
	extern uintptr_t _system_heap_start;

	/* sanity check for malformed images or loader issues */
	if (memmap->system[0].heap != (uintptr_t)&_system_heap_start)
		panic(SOF_IPC_PANIC_MEM);

	init_heap_map(memmap->system_runtime, PLATFORM_HEAP_SYSTEM_RUNTIME);

	init_heap_map(memmap->runtime, PLATFORM_HEAP_RUNTIME);

	init_heap_map(memmap->buffer, PLATFORM_HEAP_BUFFER);

#if CONFIG_DEBUG_BLOCK_FREE
	write_pattern((struct mm_heap *)&memmap->buffer, PLATFORM_HEAP_BUFFER,
		      DEBUG_BLOCK_FREE_VALUE_8BIT);
	write_pattern((struct mm_heap *)&memmap->runtime, PLATFORM_HEAP_RUNTIME,
		      DEBUG_BLOCK_FREE_VALUE_8BIT);
#endif

	spinlock_init(&memmap->lock);

	platform_shared_commit(memmap, sizeof(*memmap));
}
