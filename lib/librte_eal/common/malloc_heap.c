/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_eal_memconfig.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_common.h>
#include <rte_string_fns.h>
#include <rte_spinlock.h>
#include <rte_memcpy.h>
#include <rte_atomic.h>

#include "malloc_elem.h"
#include "malloc_heap.h"

static unsigned
check_hugepage_sz(unsigned flags, uint64_t hugepage_sz)
{
	unsigned check_flag = 0;

	if (!(flags & ~RTE_MEMZONE_SIZE_HINT_ONLY))
		return 1;

	switch (hugepage_sz) {
	case RTE_PGSIZE_256K:
		check_flag = RTE_MEMZONE_256KB;
		break;
	case RTE_PGSIZE_2M:
		check_flag = RTE_MEMZONE_2MB;
		break;
	case RTE_PGSIZE_16M:
		check_flag = RTE_MEMZONE_16MB;
		break;
	case RTE_PGSIZE_256M:
		check_flag = RTE_MEMZONE_256MB;
		break;
	case RTE_PGSIZE_512M:
		check_flag = RTE_MEMZONE_512MB;
		break;
	case RTE_PGSIZE_1G:
		check_flag = RTE_MEMZONE_1GB;
		break;
	case RTE_PGSIZE_4G:
		check_flag = RTE_MEMZONE_4GB;
		break;
	case RTE_PGSIZE_16G:
		check_flag = RTE_MEMZONE_16GB;
	}

	return check_flag & flags;
}

/*
 * Expand the heap with a memseg.
 * This reserves the zone and sets a dummy malloc_elem header at the end
 * to prevent overflow. The rest of the zone is added to free list as a single
 * large free block
 */
static void
malloc_heap_add_memseg(struct malloc_heap *heap, struct rte_memseg *ms)
{
	struct malloc_elem *start_elem = (struct malloc_elem *)ms->addr;
	const size_t elem_size = ms->len - MALLOC_ELEM_OVERHEAD;

	malloc_elem_init(start_elem, heap, ms, elem_size);
	malloc_elem_insert(start_elem);
	malloc_elem_free_list_insert(start_elem);

	heap->total_size += elem_size;
}

/*
 * Iterates through the freelist for a heap to find a free element
 * which can store data of the required size and with the requested alignment.
 * If size is 0, find the biggest available elem.
 * Returns null on failure, or pointer to element on success.
 */
static struct malloc_elem *
find_suitable_element(struct malloc_heap *heap, size_t size,
		unsigned flags, size_t align, size_t bound)
{
	size_t idx;
	struct malloc_elem *elem, *alt_elem = NULL;

	for (idx = malloc_elem_free_list_index(size);
			idx < RTE_HEAP_NUM_FREELISTS; idx++) {
		for (elem = LIST_FIRST(&heap->free_head[idx]);
				!!elem; elem = LIST_NEXT(elem, free_list)) {
			if (malloc_elem_can_hold(elem, size, align, bound)) {
				if (check_hugepage_sz(flags, elem->ms->hugepage_sz))
					return elem;
				if (alt_elem == NULL)
					alt_elem = elem;
			}
		}
	}

	if ((alt_elem != NULL) && (flags & RTE_MEMZONE_SIZE_HINT_ONLY))
		return alt_elem;

	return NULL;
}

/*
 * Main function to allocate a block of memory from the heap.
 * It locks the free list, scans it, and adds a new memseg if the
 * scan fails. Once the new memseg is added, it re-scans and should return
 * the new element after releasing the lock.
 */
void *
malloc_heap_alloc(struct malloc_heap *heap,
		const char *type __attribute__((unused)), size_t size, unsigned flags,
		size_t align, size_t bound)
{
	struct malloc_elem *elem;

	size = RTE_CACHE_LINE_ROUNDUP(size);
	align = RTE_CACHE_LINE_ROUNDUP(align);

	rte_spinlock_lock(&heap->lock);

	elem = find_suitable_element(heap, size, flags, align, bound);
	if (elem != NULL) {
		elem = malloc_elem_alloc(elem, size, align, bound);
		/* increase heap's count of allocated elements */
		heap->alloc_count++;
	}
	rte_spinlock_unlock(&heap->lock);

	return elem == NULL ? NULL : (void *)(&elem[1]);
}

int
malloc_heap_free(struct malloc_elem *elem)
{
	struct malloc_heap *heap;
	int ret;

	if (!malloc_elem_cookies_ok(elem) || elem->state != ELEM_BUSY)
		return -1;

	/* elem may be merged with previous element, so keep heap address */
	heap = elem->heap;

	rte_spinlock_lock(&(heap->lock));

	ret = malloc_elem_free(elem);

	rte_spinlock_unlock(&(heap->lock));

	return ret;
}

int
malloc_heap_resize(struct malloc_elem *elem, size_t size)
{
	int ret;

	if (!malloc_elem_cookies_ok(elem) || elem->state != ELEM_BUSY)
		return -1;

	rte_spinlock_lock(&(elem->heap->lock));

	ret = malloc_elem_resize(elem, size);

	rte_spinlock_unlock(&(elem->heap->lock));

	return ret;
}

/*
 * Function to retrieve data for heap on given socket
 */
int
malloc_heap_get_stats(struct malloc_heap *heap,
		struct rte_malloc_socket_stats *socket_stats)
{
	size_t idx;
	struct malloc_elem *elem;

	rte_spinlock_lock(&heap->lock);

	/* Initialise variables for heap */
	socket_stats->free_count = 0;
	socket_stats->heap_freesz_bytes = 0;
	socket_stats->greatest_free_size = 0;

	/* Iterate through free list */
	for (idx = 0; idx < RTE_HEAP_NUM_FREELISTS; idx++) {
		for (elem = LIST_FIRST(&heap->free_head[idx]);
			!!elem; elem = LIST_NEXT(elem, free_list))
		{
			socket_stats->free_count++;
			socket_stats->heap_freesz_bytes += elem->size;
			if (elem->size > socket_stats->greatest_free_size)
				socket_stats->greatest_free_size = elem->size;
		}
	}
	/* Get stats on overall heap and allocated memory on this heap */
	socket_stats->heap_totalsz_bytes = heap->total_size;
	socket_stats->heap_allocsz_bytes = (socket_stats->heap_totalsz_bytes -
			socket_stats->heap_freesz_bytes);
	socket_stats->alloc_count = heap->alloc_count;

	rte_spinlock_unlock(&heap->lock);
	return 0;
}

/*
 * Function to retrieve data for heap on given socket
 */
void
malloc_heap_dump(struct malloc_heap *heap, FILE *f)
{
	struct malloc_elem *elem;

	rte_spinlock_lock(&heap->lock);

	fprintf(f, "Heap size: 0x%zx\n", heap->total_size);
	fprintf(f, "Heap alloc count: %u\n", heap->alloc_count);

	elem = heap->first;
	while (elem) {
		malloc_elem_dump(elem, f);
		elem = elem->next;
	}

	rte_spinlock_unlock(&heap->lock);
}

int
rte_eal_malloc_heap_init(void)
{
	struct rte_mem_config *mcfg = rte_eal_get_configuration()->mem_config;
	unsigned ms_cnt;
	struct rte_memseg *ms;

	if (mcfg == NULL)
		return -1;

	for (ms = &mcfg->memseg[0], ms_cnt = 0;
			(ms_cnt < RTE_MAX_MEMSEG) && (ms->len > 0);
			ms_cnt++, ms++) {
		malloc_heap_add_memseg(&mcfg->malloc_heaps[ms->socket_id], ms);
	}

	return 0;
}
