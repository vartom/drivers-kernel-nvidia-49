/*
 * drivers/video/tegra/nvmap/nvmap_alloc.c
 *
 * Handle allocation and freeing routines for nvmap
 *
 * Copyright (c) 2011-2017, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/moduleparam.h>
#include <trace/events/nvmap.h>

#include <linux/version.h>

#include "nv2_heap_alloc.h"
#include "nv2_cache.h"
#include "nv2_misc.h"
#include "nv2_pp.h"
#include "nv2_dev.h"

extern bool nvmap_convert_carveout_to_iovmm;
extern bool nvmap_convert_iovmm_to_carveout;

extern u32 nvmap_max_handle_count;
extern u64 nvmap_big_page_allocs;
extern u64 nvmap_total_page_allocs;

/* small allocations will try to allocate from generic OS memory before
 * any of the limited heaps, to increase the effective memory for graphics
 * allocations, and to reduce fragmentation of the graphics heaps with
 * sub-page splinters */
static const unsigned int heap_policy_small[] = {
	NVMAP_HEAP_CARVEOUT_VPR,
	NVMAP_HEAP_CARVEOUT_IRAM,
	NVMAP_HEAP_CARVEOUT_MASK,
	NVMAP_HEAP_IOVMM,
	0,
};

static const unsigned int heap_policy_large[] = {
	NVMAP_HEAP_CARVEOUT_VPR,
	NVMAP_HEAP_CARVEOUT_IRAM,
	NVMAP_HEAP_IOVMM,
	NVMAP_HEAP_CARVEOUT_MASK,
	0,
};

static const unsigned int heap_policy_excl[] = {
	NVMAP_HEAP_CARVEOUT_IVM,
	NVMAP_HEAP_CARVEOUT_VIDMEM,
	0,
};

/*
 * set the gfp not to trigger direct/kswapd reclaims and
 * not to use emergency reserves.
 */
static gfp_t NVMAP2_heap_big_pages_gfp(gfp_t gfp)
{
	return (gfp | __GFP_NOMEMALLOC) & ~__GFP_RECLAIM;
}

unsigned int NVMAP2_heap_type_conversion(unsigned int orig_heap)
{
	unsigned int type = orig_heap;
	if (!nvmap_convert_carveout_to_iovmm
			&& nvmap_convert_iovmm_to_carveout) {
		if (type & NVMAP_HEAP_IOVMM) {
			type &= ~NVMAP_HEAP_IOVMM;
			type |= NVMAP_HEAP_CARVEOUT_GENERIC;
		}
	}
	return type;
}

int NVMAP2_heap_type_is_carveout(unsigned int heap_type)
{
	unsigned int carveout_mask = NVMAP_HEAP_CARVEOUT_MASK;

	if (nvmap_convert_carveout_to_iovmm) {
		carveout_mask &= ~NVMAP_HEAP_CARVEOUT_GENERIC;
	}
	return (heap_type & carveout_mask) ? 1 : 0;
}

int NVMAP2_heap_type_is_iovmm(unsigned int heap_type)
{
	unsigned int iovmm_mask = NVMAP_HEAP_IOVMM;

	if (nvmap_convert_carveout_to_iovmm) {
		iovmm_mask |= NVMAP_HEAP_CARVEOUT_GENERIC;
	}
	return (heap_type & iovmm_mask) ? 1 : 0;
}

static struct device *heap_pgalloc_dev(unsigned long type)
{
	int ret = -EINVAL;
	struct device *dma_dev;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	ret = 0;
#endif

	if (ret || (type != NVMAP_HEAP_CARVEOUT_VPR))
		return ERR_PTR(-EINVAL);

	dma_dev = NVMAP2_heap_type_to_dev(type);
	if (IS_ERR(dma_dev))
		return dma_dev;

	/* TODO: What consequences does this function have? */
	ret = dma_set_resizable_heap_floor_size(dma_dev, 0);
	if (ret)
		return ERR_PTR(ret);
	return dma_dev;
}

static struct page *heap_alloc_pages_exact(gfp_t gfp, size_t size)
{
	struct page *page, *p, *e;
	unsigned int order;

	order = get_order(size);
	page = alloc_pages(gfp, order);

	if (!page)
		return NULL;

	split_page(page, order);
	e = nth_page(page, (1 << order));
	for (p = nth_page(page, (size >> PAGE_SHIFT)); p < e; p++)
		__free_page(p);

	return page;
}

static int heap_big_pages_alloc_exact(struct page **pages, int starting_idx,
					gfp_t gfp, int num_pages)
{
	struct page *page;
	int idx;

	page = heap_alloc_pages_exact(gfp,
			num_pages << PAGE_SHIFT);
	if (!page)
		return -ENOMEM;

	for (idx = 0; idx < num_pages; idx++)
		pages[starting_idx + idx] = nth_page(page, idx);
	NVMAP2_cache_clean_pages(&pages[starting_idx], num_pages);

	return 0;
}

static int heap_big_pages_alloc(struct page **pages, int nr_page, gfp_t gfp)
{
	int page_index = 0;
	int pages_per_big_pg = NVMAP_PP_BIG_PAGE_SIZE >> PAGE_SHIFT;
	gfp_t gfp_no_reclaim = NVMAP2_heap_big_pages_gfp(gfp);
	int err;

#ifdef CONFIG_NVMAP_PAGE_POOLS
	/* Get as many big pages from the pool as possible. */
	page_index = nvmap_page_pool_alloc_lots_bp(&nvmap_dev->pool, pages,
			nr_page);
	pages_per_big_pg = nvmap_dev->pool.pages_per_big_pg;
#endif
	/* Try to allocate big pages from page allocator */
	for (; page_index < nr_page; page_index += pages_per_big_pg) {

		if (pages_per_big_pg < 1)
			break;
		if ((nr_page - page_index) < pages_per_big_pg)
			break;

		err = heap_big_pages_alloc_exact(pages, page_index,
				gfp_no_reclaim, pages_per_big_pg);
		if (err) {
			break;
		}
	}

	nvmap_big_page_allocs += page_index;

#ifdef CONFIG_NVMAP_PAGE_POOLS
	/* Get as many 4K pages from the pool as possible. */
	page_index += nvmap_page_pool_alloc_lots(&nvmap_dev->pool, &pages[page_index],
			nr_page - page_index);
#endif

	return page_index;
}

struct page **NVMAP2_heap_alloc_iovmm_pages(size_t size, bool contiguous)
{
	int nr_page = size >> PAGE_SHIFT;
	int i = 0, page_index = 0;
	struct page **pages;
	gfp_t gfp = GFP_NVMAP | __GFP_ZERO;

	pages = NVMAP2_altalloc(nr_page * sizeof(*pages));
	if (!pages)
		return ERR_PTR(-ENOMEM);

	if (contiguous) {
		struct page *page;
		page = heap_alloc_pages_exact(gfp, size);
		if (!page)
			goto fail;

		for (i = 0; i < nr_page; i++)
			pages[i] = nth_page(page, i);

	} else {
		page_index = heap_big_pages_alloc(pages, nr_page, gfp);

		for (i = page_index; i < nr_page; i++) {
			pages[i] = heap_alloc_pages_exact(gfp, PAGE_SIZE);
			if (!pages[i])
				goto fail;
		}
		nvmap_total_page_allocs += nr_page;
	}

	/*
	 * Make sure any data in the caches is cleaned out before
	 * passing these pages to userspace. Many nvmap clients assume that
	 * the buffers are clean as soon as they are allocated. nvmap
	 * clients can pass the buffer to hardware as it is without any
	 * explicit cache maintenance.
	 */
	if (page_index < nr_page)
		NVMAP2_cache_clean_pages(&pages[page_index], nr_page - page_index);

	return pages;

fail:
	while (i--)
		__free_page(pages[i]);
	NVMAP2_altfree(pages, nr_page * sizeof(*pages));
	wmb();
	return ERR_PTR(-ENOMEM);
}

struct page **NVMAP2_heap_alloc_dma_pages(size_t size, unsigned long type)
{
	struct page **pages;
	struct device *dma_dev;
	DEFINE_DMA_ATTRS(attrs);
	dma_addr_t pa;

	dma_dev = heap_pgalloc_dev(type);
	if (IS_ERR(dma_dev))
		return ERR_PTR(-EINVAL);

	dma_set_attr(DMA_ATTR_ALLOC_EXACT_SIZE, __DMA_ATTR(attrs));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	dma_set_attr(DMA_ATTR_ALLOC_SINGLE_PAGES, __DMA_ATTR(attrs));
#endif

	pages = dma_alloc_attrs(dma_dev, size, &pa,
			GFP_KERNEL, __DMA_ATTR(attrs));
	if (dma_mapping_error(dma_dev, pa))
		return ERR_PTR(-ENOMEM);

	return pages;
}

int NVMAP2_heap_type_is_dma(unsigned long type)
{
	struct device *dma_dev;

	dma_dev = heap_pgalloc_dev(type);
	if (IS_ERR(dma_dev))
		return 0;
	return 1;
}

void NVMAP2_heap_dealloc_dma_pages(size_t size, unsigned long type,
				struct page **pages)
{
	struct device *dma_dev;
	DEFINE_DMA_ATTRS(attrs);
	dma_addr_t pa = ~(dma_addr_t)0;

	dma_dev = heap_pgalloc_dev(type);
	if (IS_ERR(dma_dev))
		return;

	dma_set_attr(DMA_ATTR_ALLOC_EXACT_SIZE, __DMA_ATTR(attrs));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	dma_set_attr(DMA_ATTR_ALLOC_SINGLE_PAGES, __DMA_ATTR(attrs));
#endif

	dma_free_attrs(dma_dev, size, pages, pa,
		       __DMA_ATTR(attrs));
}

struct page **NVMAP2_heap_alloc_from_va(size_t size, ulong vaddr)
{
	int nr_page = size >> PAGE_SHIFT;
	struct page **pages;
	int ret = 0;

	pages = NVMAP2_altalloc(nr_page * sizeof(*pages));
	if (IS_ERR_OR_NULL(pages))
		return NULL;

	ret = NVMAP2_get_user_pages(vaddr & PAGE_MASK, nr_page, pages);
	if (ret) {
		NVMAP2_altfree(pages, nr_page * sizeof(*pages));
		return NULL;
	}

	NVMAP2_cache_clean_pages(&pages[0], nr_page);
	return pages;
}

const unsigned int *NVMAP2_heap_mask_to_policy(unsigned int heap_mask, int nr_page)
{
	const unsigned int *alloc_policy;
	int i;

	bool alloc_from_excl = false;
	/*
	 * If user specifies one of the exclusive carveouts, allocation
	 * from no other heap should be allowed.
	 */
	for (i = 0; i < ARRAY_SIZE(heap_policy_excl); i++) {
		if (!(heap_mask & heap_policy_excl[i]))
			continue;

		if (heap_mask & ~(heap_policy_excl[i])) {
			pr_err("%s alloc mixes exclusive heap %d and other heaps\n",
			       current->group_leader->comm, heap_policy_excl[i]);
			return NULL;
		}
		alloc_from_excl = true;
	}

	if (!heap_mask) {
		return NULL;
	}

	alloc_policy = alloc_from_excl ? heap_policy_excl :
			(nr_page == 1) ? heap_policy_small : heap_policy_large;
	return alloc_policy;
}

