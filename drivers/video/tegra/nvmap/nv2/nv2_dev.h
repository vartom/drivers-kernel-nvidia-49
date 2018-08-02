/*
 * Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
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

#ifndef __NVMAP2_DEV_H
#define __NVMAP2_DEV_H

#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/nvmap.h>

#include "nvmap_heap.h"
#include "nv2_pp.h"
#include "nv2_structs.h"


struct nvmap_device {
	struct rb_root	handles;
	spinlock_t	handle_lock;
	struct miscdevice dev_user;
	// TODO: heaps should be a double pointer
	struct nvmap_carveout_node *heaps;
	int nr_heaps;
	int nr_carveouts;
#ifdef CONFIG_NVMAP_PAGE_POOLS
	struct nvmap_page_pool pool;
#endif
	struct list_head clients;
	struct rb_root pids;
	struct mutex	clients_lock;
	struct list_head lru_handles;
	spinlock_t	lru_lock;
	struct dentry *handles_by_pid;
	struct dentry *debug_root;
	struct nvmap_platform_data *plat;
	struct rb_root	tags;
	struct mutex	tags_lock;
	u32 dynamic_dma_map_mask;
	u32 cpu_access_mask;
};

int nvmap_probe(struct platform_device *pdev);
int nvmap_remove(struct platform_device *pdev);
int nvmap_init(struct platform_device *pdev);

extern struct nvmap_device *nvmap_dev;

u32 NVMAP2_cpu_access_mask(void);

struct nvmap_carveout_node *NVMAP2_dev_to_carveout(struct nvmap_device *dev, int i);

static inline void NVMAP2_lru_add(struct list_head *handle_lru)
{
	spin_lock(&nvmap_dev->lru_lock);
	BUG_ON(!list_empty(handle_lru));
	list_add_tail(handle_lru, &nvmap_dev->lru_handles);
	spin_unlock(&nvmap_dev->lru_lock);
}

static inline void NVMAP2_lru_del(struct list_head *handle_lru)
{
	spin_lock(&nvmap_dev->lru_lock);
	list_del(handle_lru);
	INIT_LIST_HEAD(handle_lru);
	spin_unlock(&nvmap_dev->lru_lock);
}

static inline void NVMAP2_lru_reset(struct list_head *handle_lru)
{
	spin_lock(&nvmap_dev->lru_lock);
	BUG_ON(list_empty(handle_lru));
	list_del(handle_lru);
	list_add_tail(handle_lru, &nvmap_dev->lru_handles);
	spin_unlock(&nvmap_dev->lru_lock);
}

int nvmap_dmabuf_stash_init(void);

#endif /* __NVMAP2_DEV_H */
