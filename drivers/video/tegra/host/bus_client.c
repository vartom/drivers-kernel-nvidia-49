/*
 * Tegra Graphics Host Client Module
 *
 * Copyright (c) 2010-2015, NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/clk.h>
#include <linux/hrtimer.h>
#include <linux/export.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/tegra-soc.h>
#include <linux/anon_inodes.h>

#include <trace/events/nvhost.h>

#include <linux/io.h>
#include <linux/string.h>

#include <linux/nvhost.h>
#include <linux/nvhost_ioctl.h>

#include <mach/gpufuse.h>

#include "debug.h"
#include "bus_client.h"
#include "dev.h"
#include "class_ids.h"
#include "chip_support.h"
#include "nvhost_acm.h"
#include "nvhost_vm.h"

#include "nvhost_syncpt.h"
#include "nvhost_channel.h"
#include "nvhost_job.h"
#include "nvhost_sync.h"

static DEFINE_MUTEX(channel_lock);

int nvhost_check_bondout(unsigned int id)
{
#ifdef CONFIG_NVHOST_BONDOUT_CHECK
	if (!tegra_platform_is_silicon())
		return tegra_bonded_out_dev(id);
#endif
	return 0;
}
EXPORT_SYMBOL(nvhost_check_bondout);

static int validate_reg(struct platform_device *ndev, u32 offset, int count)
{
	int err = 0;
	struct resource *r;
	struct nvhost_device_data *pdata = platform_get_drvdata(ndev);

	r = platform_get_resource(pdata->master ? pdata->master : ndev,
			IORESOURCE_MEM, 0);
	if (!r) {
		dev_err(&ndev->dev, "failed to get memory resource\n");
		return -ENODEV;
	}

	if (offset + 4 * count > resource_size(r)
			|| (offset + 4 * count < offset))
		err = -EPERM;

	return err;
}

static __iomem void *get_aperture(struct platform_device *pdev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(pdev);

	if (pdata->master)
		pdata = platform_get_drvdata(pdata->master);

	return pdata->aperture[0];
}

void host1x_writel(struct platform_device *pdev, u32 r, u32 v)
{
	void __iomem *addr = get_aperture(pdev) + r;
	nvhost_dbg(dbg_reg, " d=%s r=0x%x v=0x%x", pdev->name, r, v);
	writel(v, addr);
}
EXPORT_SYMBOL_GPL(host1x_writel);

u32 host1x_readl(struct platform_device *pdev, u32 r)
{
	void __iomem *addr = get_aperture(pdev) + r;
	u32 v;

	nvhost_dbg(dbg_reg, " d=%s r=0x%x", pdev->name, r);
	v = readl(addr);
	nvhost_dbg(dbg_reg, " d=%s r=0x%x v=0x%x", pdev->name, r, v);

	return v;
}
EXPORT_SYMBOL_GPL(host1x_readl);

int nvhost_read_module_regs(struct platform_device *ndev,
			u32 offset, int count, u32 *values)
{
	void __iomem *p = get_aperture(ndev);
	int err;

	if (!p)
		return -ENODEV;

	/* verify offset */
	err = validate_reg(ndev, offset, count);
	if (err)
		return err;

	err = nvhost_module_busy(ndev);
	if (err)
		return err;

	p += offset;
	while (count--) {
		*(values++) = readl(p);
		p += 4;
	}
	rmb();
	nvhost_module_idle(ndev);

	return 0;
}

int nvhost_write_module_regs(struct platform_device *ndev,
			u32 offset, int count, const u32 *values)
{
	int err;
	void __iomem *p = get_aperture(ndev);

	if (!p)
		return -ENODEV;

	/* verify offset */
	err = validate_reg(ndev, offset, count);
	if (err)
		return err;

	err = nvhost_module_busy(ndev);
	if (err)
		return err;

	p += offset;
	while (count--) {
		writel(*(values++), p);
		p += 4;
	}
	wmb();
	nvhost_module_idle(ndev);

	return 0;
}

struct nvhost_channel_userctx {
	struct nvhost_channel *ch;
	u32 timeout;
	u32 priority;
	int clientid;
	bool timeout_debug_dump;
	struct platform_device *pdev;
	u32 syncpts[NVHOST_MODULE_MAX_SYNCPTS];
	u32 client_managed_syncpt;

	/* error notificatiers used channel submit timeout */
	struct dma_buf *error_notifier_ref;
	u64 error_notifier_offset;

	/* lock to protect this structure from concurrent ioctl usage */
	struct mutex ioctl_lock;

	/* context address space */
	struct nvhost_vm *vm;
};

static int nvhost_channelrelease(struct inode *inode, struct file *filp)
{
	struct nvhost_channel_userctx *priv = filp->private_data;
	struct nvhost_device_data *pdata = platform_get_drvdata(priv->pdev);
	int i = 0;

	trace_nvhost_channel_release(dev_name(&priv->pdev->dev));

	mutex_lock(&channel_lock);

	/* remove this client from acm */
	nvhost_module_remove_client(priv->pdev, priv);

	/* drop error notifier reference */
	if (priv->error_notifier_ref)
		dma_buf_put(priv->error_notifier_ref);

	nvhost_vm_put(priv->vm);

	/* If the device is in exclusive mode, drop the reference */
	if (nvhost_get_channel_policy() == MAP_CHANNEL_ON_SUBMIT &&
	    pdata->exclusive)
		pdata->num_mapped_chs--;

	mutex_unlock(&channel_lock);

	/* drop channel reference if we took one at open time */
	if (nvhost_get_channel_policy() == MAP_CHANNEL_ON_OPEN)
		nvhost_putchannel(priv->ch, 1);

	if (nvhost_get_syncpt_policy() == SYNCPT_PER_CHANNEL_INSTANCE) {
		/* Release instance syncpoints */
		for (i = 0; i < NVHOST_MODULE_MAX_SYNCPTS; ++i) {
			if (priv->syncpts[i]) {
				nvhost_free_syncpt(priv->syncpts[i]);
				priv->syncpts[i] = 0;
			}
		}

		if (priv->client_managed_syncpt) {
			nvhost_free_syncpt(priv->client_managed_syncpt);
			priv->client_managed_syncpt = 0;
		}
	}

	if (pdata->keepalive)
		nvhost_module_enable_poweroff(priv->pdev);

	kfree(priv);
	return 0;
}

static int __nvhost_channelopen(struct inode *inode,
		struct platform_device *pdev,
		struct file *filp)
{
	struct nvhost_channel_userctx *priv;
	struct nvhost_device_data *pdata, *host1x_pdata;
	struct nvhost_channel *ch = NULL;
	int ret;

	/* grab pdev and pdata based on inputs */
	if (pdev) {
		pdata = platform_get_drvdata(pdev);
	} else if (inode) {
		pdata = container_of(inode->i_cdev,
				struct nvhost_device_data, cdev);
		pdev = pdata->pdev;
	} else
		return -EINVAL;

	/* ..and host1x platform data */
	host1x_pdata = dev_get_drvdata(pdev->dev.parent);

	/* get a channel if we are in map-at-open -mode */
	if (nvhost_get_channel_policy() == MAP_CHANNEL_ON_OPEN) {
		ret = nvhost_channel_map(pdata, &ch, NULL);
		if (ret || !ch) {
			pr_err("%s: failed to map channel, error: %d\n",
			       __func__, ret);
			return ret;
		}
	}

	trace_nvhost_channel_open(dev_name(&pdev->dev));

	mutex_lock(&channel_lock);

	/* If the device is in exclusive mode, make channel reservation here */
	if (nvhost_get_channel_policy() == MAP_CHANNEL_ON_SUBMIT &&
	    pdata->exclusive) {
		if (pdata->num_mapped_chs == pdata->num_channels)
			goto fail;
		pdata->num_mapped_chs++;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		goto fail;
	filp->private_data = priv;

	/* Register this client to acm */
	if (nvhost_module_add_client(pdev, priv))
		goto fail_add_client;

	/* Keep devices with keepalive flag powered */
	if (pdata->keepalive)
		nvhost_module_disable_poweroff(pdev);

	/* Check that the device can be powered */
	ret = nvhost_module_busy(pdev);
	if (ret)
		goto fail_power_on;
	nvhost_module_idle(pdev);

	/* Get client id */
	priv->clientid = atomic_add_return(1,
			&nvhost_get_host(pdev)->clientid);
	if (!priv->clientid)
		priv->clientid = atomic_add_return(1,
				&nvhost_get_host(pdev)->clientid);

	/* Initialize private structure */
	priv->timeout = host1x_pdata->nvhost_timeout_default;
	priv->priority = NVHOST_PRIORITY_MEDIUM;
	priv->timeout_debug_dump = true;
	mutex_init(&priv->ioctl_lock);
	priv->pdev = pdev;
	priv->ch = ch;

	if (!tegra_platform_is_silicon())
		priv->timeout = 0;

	priv->vm = nvhost_vm_allocate(pdev);
	if (!priv->vm)
		goto fail_alloc_vm;

	mutex_unlock(&channel_lock);

	return 0;

fail_alloc_vm:
fail_power_on:
fail_add_client:
	kfree(priv);

fail:
	if (ch)
		nvhost_putchannel(ch, 1);
	mutex_unlock(&channel_lock);

	return -ENOMEM;
}

static int nvhost_channelopen(struct inode *inode, struct file *filp)
{
	return __nvhost_channelopen(inode, NULL, filp);
}

static int nvhost_init_error_notifier(struct nvhost_channel_userctx *ctx,
				      struct nvhost_set_error_notifier *args)
{
	struct dma_buf *dmabuf;
	void *va;

	/* are we releasing old reference? */
	if (!args->mem) {
		if (ctx->error_notifier_ref)
			dma_buf_put(ctx->error_notifier_ref);
		ctx->error_notifier_ref = NULL;
		return 0;
	}

	/* take reference for the userctx */
	dmabuf = dma_buf_get(args->mem);
	if (IS_ERR(dmabuf)) {
		pr_err("%s: Invalid handle: %d\n", __func__, args->mem);
		return -EINVAL;
	}

	/* map handle and clear error notifier struct */
	va = dma_buf_vmap(dmabuf);
	if (!va) {
		dma_buf_put(dmabuf);
		pr_err("%s: Cannot map notifier handle\n", __func__);
		return -ENOMEM;
	}

	memset(va + args->offset, 0, sizeof(struct nvhost_notification));
	dma_buf_vunmap(dmabuf, va);

	/* release old reference */
	if (ctx->error_notifier_ref)
		dma_buf_put(ctx->error_notifier_ref);

	/* finally, store error notifier data */
	ctx->error_notifier_ref = dmabuf;
	ctx->error_notifier_offset = args->offset;

	return 0;
}

static inline u32 get_job_fence(struct nvhost_job *job, u32 id)
{
	struct nvhost_channel *ch = job->ch;
	struct nvhost_device_data *pdata = platform_get_drvdata(ch->dev);
	u32 fence = job->sp[id].fence;

	/* take into account work done increment */
	if (pdata->push_work_done && id == 0)
		return fence - 1;

	/* otherwise the fence is valid "as is" */
	return fence;
}

static int nvhost_ioctl_channel_submit(struct nvhost_channel_userctx *ctx,
		struct nvhost_submit_args *args)
{
	struct nvhost_job *job;
	int num_cmdbufs = args->num_cmdbufs;
	int num_relocs = args->num_relocs;
	int num_waitchks = args->num_waitchks;
	int num_syncpt_incrs = args->num_syncpt_incrs;
	struct nvhost_cmdbuf __user *cmdbufs =
		(struct nvhost_cmdbuf __user *)(uintptr_t)args->cmdbufs;
	struct nvhost_cmdbuf __user *cmdbuf_exts =
		(struct nvhost_cmdbuf __user *)(uintptr_t)args->cmdbuf_exts;
	struct nvhost_reloc __user *relocs =
		(struct nvhost_reloc __user *)(uintptr_t)args->relocs;
	struct nvhost_reloc_shift __user *reloc_shifts =
		(struct nvhost_reloc_shift __user *)
				(uintptr_t)args->reloc_shifts;
	struct nvhost_waitchk __user *waitchks =
		(struct nvhost_waitchk __user *)(uintptr_t)args->waitchks;
	struct nvhost_syncpt_incr __user *syncpt_incrs =
		(struct nvhost_syncpt_incr __user *)
				(uintptr_t)args->syncpt_incrs;
	u32 __user *fences = (u32 __user *)(uintptr_t)args->fences;
	u32 __user *class_ids = (u32 __user *)(uintptr_t)args->class_ids;
	struct nvhost_device_data *pdata = platform_get_drvdata(ctx->pdev);

	struct nvhost_master *host = nvhost_get_host(ctx->pdev);
	const u32 *syncpt_array =
		(nvhost_get_syncpt_policy() == SYNCPT_PER_CHANNEL_INSTANCE) ?
		ctx->syncpts :
		ctx->ch->syncpts;
	u32 *local_class_ids = NULL;
	int err, i;

	if (num_syncpt_incrs > host->info.nb_pts)
		return -EINVAL;

	job = nvhost_job_alloc(ctx->ch,
			num_cmdbufs,
			num_relocs,
			num_waitchks,
			num_syncpt_incrs);
	if (!job)
		return -ENOMEM;

	job->num_relocs = args->num_relocs;
	job->num_waitchk = args->num_waitchks;
	job->num_syncpts = args->num_syncpt_incrs;
	job->priority = ctx->priority;
	job->clientid = ctx->clientid;
	job->vm = ctx->vm;
	nvhost_vm_get(job->vm);

	/* copy error notifier settings for this job */
	if (ctx->error_notifier_ref) {
		get_dma_buf(ctx->error_notifier_ref);
		job->error_notifier_ref = ctx->error_notifier_ref;
		job->error_notifier_offset = ctx->error_notifier_offset;
	}

	/* mass copy class_ids */
	if (args->class_ids) {
		local_class_ids = kzalloc(sizeof(u32) * num_cmdbufs,
			GFP_KERNEL);
		if (!local_class_ids) {
			err = -ENOMEM;
			goto fail;
		}
		err = copy_from_user(local_class_ids, class_ids,
			sizeof(u32) * num_cmdbufs);
		if (err) {
			err = -EINVAL;
			goto fail;
		}
	}

	for (i = 0; i < num_cmdbufs; ++i) {
		struct nvhost_cmdbuf cmdbuf;
		struct nvhost_cmdbuf_ext cmdbuf_ext;
		u32 class_id = class_ids ? local_class_ids[i] : 0;

		err = copy_from_user(&cmdbuf, cmdbufs + i, sizeof(cmdbuf));
		if (err)
			goto fail;

		cmdbuf_ext.pre_fence = -1;
		if (cmdbuf_exts)
			err = copy_from_user(&cmdbuf_ext,
					cmdbuf_exts + i, sizeof(cmdbuf_ext));
		if (err)
			cmdbuf_ext.pre_fence = -1;

		/* verify that the given class id is valid for this engine */
		if (class_id &&
		    class_id != pdata->class &&
		    class_id != NV_HOST1X_CLASS_ID) {
			err = -EINVAL;
			goto fail;
		}

		nvhost_job_add_gather(job, cmdbuf.mem, cmdbuf.words,
				      cmdbuf.offset, class_id,
				      cmdbuf_ext.pre_fence);
	}

	kfree(local_class_ids);
	local_class_ids = NULL;

	err = copy_from_user(job->relocarray,
			relocs, sizeof(*relocs) * num_relocs);
	if (err)
		goto fail;

	err = copy_from_user(job->relocshiftarray,
			reloc_shifts, sizeof(*reloc_shifts) * num_relocs);
	if (err)
		goto fail;

	err = copy_from_user(job->waitchk,
			waitchks, sizeof(*waitchks) * num_waitchks);
	if (err)
		goto fail;

	/*
	 * Go through each syncpoint from userspace. Here we:
	 * - Copy syncpoint information
	 * - Validate each syncpoint
	 * - Determine the index of hwctx syncpoint in the table
	 */

	for (i = 0; i < num_syncpt_incrs; ++i) {
		struct nvhost_syncpt_incr sp;
		bool found = false;
		int j;

		/* Copy */
		err = copy_from_user(&sp, syncpt_incrs + i, sizeof(sp));
		if (err)
			goto fail;

		/* Validate the trivial case */
		if (sp.syncpt_id == 0) {
			err = -EINVAL;
			goto fail;
		}

		/* ..and then ensure that the syncpoints have been reserved
		 * for this client */
		for (j = 0; j < NVHOST_MODULE_MAX_SYNCPTS; j++) {
			if (syncpt_array[j] == sp.syncpt_id) {
				found = true;
				break;
			}
		}

		if (!found) {
			err = -EINVAL;
			goto fail;
		}

		/* Store */
		job->sp[i].id = sp.syncpt_id;
		job->sp[i].incrs = sp.syncpt_incrs;
	}

	job->hwctx_syncpt_idx = 0;

	trace_nvhost_channel_submit(ctx->pdev->name,
		job->num_gathers, job->num_relocs, job->num_waitchk,
		job->sp[job->hwctx_syncpt_idx].id,
		job->sp[job->hwctx_syncpt_idx].incrs);

	err = nvhost_module_busy(ctx->pdev);
	if (err)
		goto fail;

	err = nvhost_job_pin(job, &nvhost_get_host(ctx->pdev)->syncpt);
	nvhost_module_idle(ctx->pdev);
	if (err)
		goto fail;

	if (args->timeout)
		job->timeout = min(ctx->timeout, args->timeout);
	else
		job->timeout = ctx->timeout;
	job->timeout_debug_dump = ctx->timeout_debug_dump;

	err = nvhost_channel_submit(job);
	if (err)
		goto fail_submit;

	/* Deliver multiple fences back to the userspace */
	if (fences)
		for (i = 0; i < num_syncpt_incrs; ++i) {
			u32 fence = get_job_fence(job, i);
			err = copy_to_user(fences, &fence, sizeof(u32));
			if (err)
				break;
			fences++;
		}

	/* Deliver the fence using the old mechanism _only_ if a single
	 * syncpoint is used. */

	if (args->flags & BIT(NVHOST_SUBMIT_FLAG_SYNC_FENCE_FD)) {
		struct nvhost_ctrl_sync_fence_info pts[num_syncpt_incrs];

		for (i = 0; i < num_syncpt_incrs; i++) {
			pts[i].id = job->sp[i].id;
			pts[i].thresh = get_job_fence(job, i);
		}

		err = nvhost_sync_create_fence_fd(ctx->pdev,
				pts, num_syncpt_incrs, "fence", &args->fence);
		if (err)
			goto fail;
	} else if (num_syncpt_incrs == 1)
		args->fence =  get_job_fence(job, job->hwctx_syncpt_idx);
	else
		args->fence = 0;

	nvhost_job_put(job);

	return 0;

fail_submit:
	nvhost_job_unpin(job);
fail:
	nvhost_job_put(job);
	kfree(local_class_ids);
	return err;
}

static int nvhost_ioctl_channel_map_buffer(struct nvhost_channel_userctx *ctx,
				struct nvhost_channel_map_buffer_args *args)
{
	struct nvhost_channel_buffer __user *__buffers =
		(struct nvhost_channel_buffer __user *)
		(uintptr_t)args->table_address;
	struct nvhost_channel_buffer *buffers;
	int err = 0, i = 0, num_handled_buffers = 0;
	dma_addr_t addr = 0;

	/* ensure that reserved fields are kept clear */
	if (args->reserved)
		return -EINVAL;

	/* allocate room for buffers */
	buffers = kzalloc(args->num_buffers * sizeof(*buffers), GFP_KERNEL);
	if (!buffers) {
		err = -ENOMEM;
		goto err_alloc_buffers;
	}

	/* copy the buffers from user space */
	err = copy_from_user(buffers, __buffers,
			     sizeof(*__buffers) * args->num_buffers);
	if (err)
		goto err_copy_from_user;

	/* go through all the buffers */
	for (i = 0, num_handled_buffers = 0;
	     i < args->num_buffers;
	     i++, num_handled_buffers++) {
		struct dma_buf *dmabuf;

		/* ensure that reserved fields are kept clear */
		if (buffers[i].reserved0 ||
		    buffers[i].reserved1[0] ||
		    buffers[i].reserved1[1]) {
			err = -EINVAL;
			goto err_map_buffers;
		}

		/* validate dmabuf fd */
		dmabuf = dma_buf_get(buffers[i].dmabuf_fd);
		if (IS_ERR(dmabuf)) {
			err = PTR_ERR(dmabuf);
			goto err_map_buffers;
		}

		/* map it into context vm */
		err = nvhost_vm_map_dmabuf(ctx->vm, dmabuf,
					   &addr);
		buffers[i].address = (u64)addr;

		/* not needed anymore, vm keeps reference now */
		dma_buf_put(dmabuf);

		if (err)
			goto err_map_buffers;
	}

	/* finally, copy the addresses back to userspace */
	err = copy_to_user(__buffers, buffers,
			   args->num_buffers * sizeof(*buffers));
	if (err)
		goto err_copy_buffers_to_user;

	kfree(buffers);
	return err;

err_copy_buffers_to_user:
err_map_buffers:
	for (i = 0; i < num_handled_buffers; i++) {
		struct dma_buf *dmabuf;

		dmabuf = dma_buf_get(buffers[i].dmabuf_fd);
		if (IS_ERR(dmabuf))
			continue;
		nvhost_vm_unmap_dmabuf(ctx->vm, dmabuf);
		dma_buf_put(dmabuf);
	}
err_copy_from_user:
	kfree(buffers);
err_alloc_buffers:
	return err;
}

static int nvhost_ioctl_channel_unmap_buffer(struct nvhost_channel_userctx *ctx,
				struct nvhost_channel_unmap_buffer_args *args)
{
	struct nvhost_channel_buffer __user *__buffers =
		(struct nvhost_channel_buffer __user *)
		(uintptr_t)args->table_address;
	struct nvhost_channel_buffer *buffers;
	int err = 0, i = 0, num_handled_buffers = 0;
	struct dma_buf **dmabufs;

	/* ensure that reserved fields are kept clear */
	if (args->reserved)
		return -EINVAL;

	/* allocate room for buffers */
	buffers = kzalloc(args->num_buffers * sizeof(*buffers), GFP_KERNEL);
	if (!buffers) {
		err = -ENOMEM;
		goto err_alloc_buffers;
	}

	/* allocate room for buffers */
	dmabufs = kzalloc(args->num_buffers * sizeof(*dmabufs), GFP_KERNEL);
	if (!dmabufs) {
		err = -ENOMEM;
		goto err_alloc_dmabufs;
	}

	/* copy the buffers from user space */
	err = copy_from_user(buffers, __buffers,
			     sizeof(*__buffers) * args->num_buffers);
	if (err)
		goto err_copy_from_user;

	/* first get all dmabufs... */
	for (i = 0, num_handled_buffers = 0;
	     i < args->num_buffers;
	     i++, num_handled_buffers++) {
		/* ensure that reserved fields are kept clear */
		if (buffers[i].reserved0 ||
		    buffers[i].reserved1[0] ||
		    buffers[i].reserved1[1]) {
			err = -EINVAL;
			goto err_get_dmabufs;
		}

		dmabufs[i] = dma_buf_get(buffers[i].dmabuf_fd);
		if (IS_ERR(dmabufs[i])) {
			err = PTR_ERR(dmabufs[i]);
			goto err_get_dmabufs;
		}
	}

	/* ..then unmap */
	for (i = 0; i < args->num_buffers; i++)
		nvhost_vm_unmap_dmabuf(ctx->vm, dmabufs[i]);

err_get_dmabufs:
	for (i = 0; i < num_handled_buffers; i++)
		dma_buf_put(dmabufs[i]);
err_copy_from_user:
	kfree(dmabufs);
err_alloc_dmabufs:
	kfree(buffers);
err_alloc_buffers:
	return err;
}

static int moduleid_to_index(struct platform_device *dev, u32 moduleid)
{
	int i;
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);

	for (i = 0; i < NVHOST_MODULE_MAX_CLOCKS; i++) {
		if (pdata->clocks[i].moduleid == moduleid)
			return i;
	}

	/* Old user space is sending a random number in args. Return clock
	 * zero in these cases. */
	return 0;
}

static int nvhost_ioctl_channel_set_rate(struct nvhost_channel_userctx *ctx,
	struct nvhost_clk_rate_args *arg)
{
	u32 moduleid = (arg->moduleid >> NVHOST_MODULE_ID_BIT_POS)
			& ((1 << NVHOST_MODULE_ID_BIT_WIDTH) - 1);
	u32 attr = (arg->moduleid >> NVHOST_CLOCK_ATTR_BIT_POS)
			& ((1 << NVHOST_CLOCK_ATTR_BIT_WIDTH) - 1);
	int index = moduleid ?
			moduleid_to_index(ctx->pdev, moduleid) : 0;

	return nvhost_module_set_rate(ctx->pdev,
			ctx, arg->rate, index, attr);
}

static int nvhost_ioctl_channel_get_rate(struct nvhost_channel_userctx *ctx,
	u32 moduleid, u32 *rate)
{
	int index = moduleid ? moduleid_to_index(ctx->pdev, moduleid) : 0;

	return nvhost_module_get_rate(ctx->pdev,
			(unsigned long *)rate, index);
}

static int nvhost_ioctl_channel_module_regrdwr(
	struct nvhost_channel_userctx *ctx,
	struct nvhost_ctrl_module_regrdwr_args *args)
{
	u32 num_offsets = args->num_offsets;
	u32 __user *offsets = (u32 __user *)(uintptr_t)args->offsets;
	u32 __user *values = (u32 __user *)(uintptr_t)args->values;
	u32 vals[64];
	struct platform_device *ndev;

	trace_nvhost_ioctl_channel_module_regrdwr(args->id,
		args->num_offsets, args->write);

	/* Check that there is something to read and that block size is
	 * u32 aligned */
	if (num_offsets == 0 || args->block_size & 3)
		return -EINVAL;

	ndev = ctx->pdev;

	while (num_offsets--) {
		int err;
		u32 offs;
		int remaining = args->block_size >> 2;

		if (get_user(offs, offsets))
			return -EFAULT;

		offsets++;
		while (remaining) {
			int batch = min(remaining, 64);
			if (args->write) {
				if (copy_from_user(vals, values,
						batch * sizeof(u32)))
					return -EFAULT;

				err = nvhost_write_module_regs(ndev,
					offs, batch, vals);
				if (err)
					return err;
			} else {
				err = nvhost_read_module_regs(ndev,
						offs, batch, vals);
				if (err)
					return err;

				if (copy_to_user(values, vals,
						batch * sizeof(u32)))
					return -EFAULT;
			}

			remaining -= batch;
			offs += batch * sizeof(u32);
			values += batch;
		}
	}

	return 0;
}

static u32 create_mask(u32 *words, int num)
{
	int i;
	u32 word = 0;
	for (i = 0; i < num; i++) {
		if (!words[i] || words[i] > 31)
			continue;
		word |= BIT(words[i]);
	}

	return word;
}

static u32 nvhost_ioctl_channel_get_syncpt_mask(
		struct nvhost_channel_userctx *priv)
{
	u32 mask;

	if (nvhost_get_syncpt_policy() == SYNCPT_PER_CHANNEL_INSTANCE)
		mask = create_mask(priv->syncpts, NVHOST_MODULE_MAX_SYNCPTS);
	else
		mask = create_mask(priv->ch->syncpts,
						NVHOST_MODULE_MAX_SYNCPTS);

	return mask;
}

static u32 nvhost_ioctl_channel_get_syncpt_channel(struct nvhost_channel *ch,
		struct nvhost_device_data *pdata, u32 index)
{
	u32 id;

	mutex_lock(&ch->syncpts_lock);

	/* if we already have required syncpt then return it ... */
	id = ch->syncpts[index];
	if (id)
		goto exit_unlock;

	/* ... otherwise get a new syncpt dynamically */
	id = nvhost_get_syncpt_host_managed(pdata->pdev, index);
	if (!id)
		goto exit_unlock;

	/* ... and store it for further references */
	ch->syncpts[index] = id;

exit_unlock:
	mutex_unlock(&ch->syncpts_lock);
	return id;
}

static u32 nvhost_ioctl_channel_get_syncpt_instance(
		struct nvhost_channel_userctx *ctx,
		struct nvhost_device_data *pdata, u32 index)
{
	u32 id;

	/* if we already have required syncpt then return it ... */
	if (ctx->syncpts[index]) {
		id = ctx->syncpts[index];
		return id;
	}

	/* ... otherwise get a new syncpt dynamically */
	id = nvhost_get_syncpt_host_managed(pdata->pdev, index);
	if (!id)
		return 0;

	/* ... and store it for further references */
	ctx->syncpts[index] = id;

	return id;
}

static int nvhost_ioctl_channel_get_client_syncpt(
		struct nvhost_channel_userctx *ctx,
		struct nvhost_get_client_managed_syncpt_arg *args)
{
	const char __user *args_name =
		(const char __user *)(uintptr_t)args->name;
	char name[32];
	char set_name[32];

	/* prepare syncpoint name (in case it is needed) */
	if (args_name) {
		if (strncpy_from_user(name, args_name, sizeof(name)) < 0)
			return -EFAULT;
		name[sizeof(name) - 1] = '\0';
	} else {
		name[0] = '\0';
	}

	snprintf(set_name, sizeof(set_name),
		"%s_%s", dev_name(&ctx->pdev->dev), name);

	if (nvhost_get_syncpt_policy() == SYNCPT_PER_CHANNEL_INSTANCE) {
		if (!ctx->client_managed_syncpt)
			ctx->client_managed_syncpt =
				nvhost_get_syncpt_client_managed(set_name);
		args->value = ctx->client_managed_syncpt;
	} else {
		struct nvhost_channel *ch = ctx->ch;
		mutex_lock(&ch->syncpts_lock);
		if (!ch->client_managed_syncpt)
			ch->client_managed_syncpt =
				nvhost_get_syncpt_client_managed(set_name);
		mutex_unlock(&ch->syncpts_lock);
		args->value = ch->client_managed_syncpt;
	}

	if (!args->value)
		return -EAGAIN;

	return 0;
}

static long nvhost_channelctl(struct file *filp,
	unsigned int cmd, unsigned long arg)
{
	struct nvhost_channel_userctx *priv = filp->private_data;
	struct device *dev;
	u8 buf[NVHOST_IOCTL_CHANNEL_MAX_ARG_SIZE];
	int err = 0;

	if ((_IOC_TYPE(cmd) != NVHOST_IOCTL_MAGIC) ||
		(_IOC_NR(cmd) == 0) ||
		(_IOC_NR(cmd) > NVHOST_IOCTL_CHANNEL_LAST) ||
		(_IOC_SIZE(cmd) > NVHOST_IOCTL_CHANNEL_MAX_ARG_SIZE))
		return -EFAULT;

	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		if (copy_from_user(buf, (void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	/* serialize calls from this fd */
	mutex_lock(&priv->ioctl_lock);
	if (!priv->pdev) {
		pr_warn("Channel already unmapped\n");
		mutex_unlock(&priv->ioctl_lock);
		return -EFAULT;
	}

	dev = &priv->pdev->dev;
	switch (cmd) {
	case NVHOST_IOCTL_CHANNEL_OPEN:
	{
		int fd;
		struct file *file;
		char *name;

		err = get_unused_fd_flags(O_RDWR);
		if (err < 0)
			break;
		fd = err;

		name = kasprintf(GFP_KERNEL, "nvhost-%s-fd%d",
				dev_name(dev), fd);
		if (!name) {
			err = -ENOMEM;
			put_unused_fd(fd);
			break;
		}

		file = anon_inode_getfile(name, filp->f_op, NULL, O_RDWR);
		kfree(name);
		if (IS_ERR(file)) {
			err = PTR_ERR(file);
			put_unused_fd(fd);
			break;
		}
		fd_install(fd, file);

		err = __nvhost_channelopen(NULL, priv->pdev, file);
		if (err) {
			put_unused_fd(fd);
			fput(file);
			break;
		}

		((struct nvhost_channel_open_args *)buf)->channel_fd = fd;
		break;
	}
	case NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS:
	{
		((struct nvhost_get_param_args *)buf)->value =
			nvhost_ioctl_channel_get_syncpt_mask(priv);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT:
	{
		struct nvhost_device_data *pdata =
			platform_get_drvdata(priv->pdev);
		struct nvhost_get_param_arg *arg =
			(struct nvhost_get_param_arg *)buf;

		if (arg->param >= NVHOST_MODULE_MAX_SYNCPTS) {
			err = -EINVAL;
			break;
		}

		if (nvhost_get_syncpt_policy() == SYNCPT_PER_CHANNEL_INSTANCE)
			arg->value = nvhost_ioctl_channel_get_syncpt_instance(
						priv, pdata, arg->param);
		else
			arg->value = nvhost_ioctl_channel_get_syncpt_channel(
						priv->ch, pdata, arg->param);
		if (!arg->value) {
			err = -EAGAIN;
			break;
		}
		break;
	}
	case NVHOST_IOCTL_CHANNEL_GET_CLIENT_MANAGED_SYNCPOINT:
	{
		err = nvhost_ioctl_channel_get_client_syncpt(priv,
			(struct nvhost_get_client_managed_syncpt_arg *)buf);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_FREE_CLIENT_MANAGED_SYNCPOINT:
		break;
	case NVHOST_IOCTL_CHANNEL_GET_WAITBASES:
	{
		((struct nvhost_get_param_args *)buf)->value = 0;
		break;
	}
	case NVHOST_IOCTL_CHANNEL_GET_WAITBASE:
	{
		err = -EINVAL;
		break;
	}
	case NVHOST_IOCTL_CHANNEL_GET_MODMUTEXES:
	{
		struct nvhost_device_data *pdata = \
			platform_get_drvdata(priv->pdev);
		((struct nvhost_get_param_args *)buf)->value =
			create_mask(pdata->modulemutexes,
					NVHOST_MODULE_MAX_MODMUTEXES);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_GET_MODMUTEX:
	{
		struct nvhost_device_data *pdata = \
			platform_get_drvdata(priv->pdev);
		struct nvhost_get_param_arg *arg =
			(struct nvhost_get_param_arg *)buf;

		if (arg->param >= NVHOST_MODULE_MAX_MODMUTEXES ||
		    !pdata->modulemutexes[arg->param]) {
			err = -EINVAL;
			break;
		}

		arg->value = pdata->modulemutexes[arg->param];
		break;
	}
	case NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD:
		break;
	case NVHOST_IOCTL_CHANNEL_GET_CLK_RATE_LEGACY:
	case NVHOST_IOCTL_CHANNEL_GET_CLK_RATE:
	{
		struct nvhost_clk_rate_args *arg =
				(struct nvhost_clk_rate_args *)buf;

		err = nvhost_ioctl_channel_get_rate(priv,
				arg->moduleid, &arg->rate);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_SET_CLK_RATE:
	{
		struct nvhost_clk_rate_args *arg =
				(struct nvhost_clk_rate_args *)buf;

		err = nvhost_ioctl_channel_set_rate(priv, arg);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_SET_TIMEOUT:
	{
		u32 timeout =
			(u32)((struct nvhost_set_timeout_args *)buf)->timeout;

		priv->timeout = timeout;
		dev_dbg(&priv->pdev->dev,
			"%s: setting buffer timeout (%d ms) for userctx 0x%p\n",
			__func__, priv->timeout, priv);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_GET_TIMEDOUT:
		((struct nvhost_get_param_args *)buf)->value = false;
		break;
	case NVHOST_IOCTL_CHANNEL_SET_PRIORITY:
		priv->priority =
			(u32)((struct nvhost_set_priority_args *)buf)->priority;
		break;
	case NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR:
	{
		struct nvhost32_ctrl_module_regrdwr_args *args32 =
			(struct nvhost32_ctrl_module_regrdwr_args *)buf;
		struct nvhost_ctrl_module_regrdwr_args args;
		args.id = args32->id;
		args.num_offsets = args32->num_offsets;
		args.block_size = args32->block_size;
		args.offsets = args32->offsets;
		args.values = args32->values;
		args.write = args32->write;
		err = nvhost_ioctl_channel_module_regrdwr(priv, &args);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_MODULE_REGRDWR:
		err = nvhost_ioctl_channel_module_regrdwr(priv, (void *)buf);
		break;
	case NVHOST32_IOCTL_CHANNEL_SUBMIT:
	{
		struct nvhost_device_data *pdata =
			platform_get_drvdata(priv->pdev);
		struct nvhost32_submit_args *args32 = (void *)buf;
		struct nvhost_submit_args args;

		memset(&args, 0, sizeof(args));
		args.submit_version = args32->submit_version;
		args.num_syncpt_incrs = args32->num_syncpt_incrs;
		args.num_cmdbufs = args32->num_cmdbufs;
		args.num_relocs = args32->num_relocs;
		args.num_waitchks = args32->num_waitchks;
		args.timeout = args32->timeout;
		args.syncpt_incrs = args32->syncpt_incrs;
		args.fence = args32->fence;

		args.cmdbufs = args32->cmdbufs;
		args.relocs = args32->relocs;
		args.reloc_shifts = args32->reloc_shifts;
		args.waitchks = args32->waitchks;
		args.class_ids = args32->class_ids;
		args.fences = args32->fences;

		if (nvhost_get_channel_policy() == MAP_CHANNEL_ON_SUBMIT) {
			/* first, get a channel */
			err = nvhost_channel_map(pdata, &priv->ch, priv);
			if (err)
				break;

			/* ..then, synchronize syncpoint information.
			 *
			 * This information is updated only in this ioctl and
			 * channel destruction. We already hold channel
			 * reference and this ioctl is serialized => no-one is
			 * modifying the syncpoint field concurrently.
			 *
			 * Synchronization is not destructing anything
			 * in the structure; We can only allocate new
			 * syncpoints, and hence old ones cannot be released
			 * by following operation. If some syncpoint is stored
			 * into the channel structure, it remains there. */

			memcpy(priv->ch->syncpts, priv->syncpts,
			       sizeof(priv->syncpts));
			priv->ch->client_managed_syncpt =
				priv->client_managed_syncpt;

			/* submit work */
			err = nvhost_ioctl_channel_submit(priv, &args);

			/* ..and drop the local reference */
			nvhost_putchannel(priv->ch, 1);
		} else {
			err = nvhost_ioctl_channel_submit(priv, &args);
		}
		args32->fence = args.fence;

		break;
	}
	case NVHOST_IOCTL_CHANNEL_SUBMIT:
	{
		struct nvhost_device_data *pdata =
			platform_get_drvdata(priv->pdev);

		if (nvhost_get_channel_policy() == MAP_CHANNEL_ON_SUBMIT) {
			/* first, get a channel */
			err = nvhost_channel_map(pdata, &priv->ch, priv);
			if (err)
				break;

			/* ..then, synchronize syncpoint information.
			 *
			 * This information is updated only in this ioctl and
			 * channel destruction. We already hold channel
			 * reference and this ioctl is serialized => no-one is
			 * modifying the syncpoint field concurrently.
			 *
			 * Synchronization is not destructing anything
			 * in the structure; We can only allocate new
			 * syncpoints, and hence old ones cannot be released
			 * by following operation. If some syncpoint is stored
			 * into the channel structure, it remains there. */

			memcpy(priv->ch->syncpts, priv->syncpts,
			       sizeof(priv->syncpts));
			priv->ch->client_managed_syncpt =
				priv->client_managed_syncpt;

			/* submit work */
			err = nvhost_ioctl_channel_submit(priv, (void *)buf);

			/* ..and drop the local reference */
			nvhost_putchannel(priv->ch, 1);
		} else {
			err = nvhost_ioctl_channel_submit(priv, (void *)buf);
		}

		break;
	}
	case NVHOST_IOCTL_CHANNEL_SET_ERROR_NOTIFIER:
		err = nvhost_init_error_notifier(priv,
			(struct nvhost_set_error_notifier *)buf);
		break;
	case NVHOST_IOCTL_CHANNEL_MAP_BUFFER:
		err = nvhost_ioctl_channel_map_buffer(priv, (void *)buf);
		break;
	case NVHOST_IOCTL_CHANNEL_UNMAP_BUFFER:
		err = nvhost_ioctl_channel_unmap_buffer(priv, (void *)buf);
		break;
	case NVHOST_IOCTL_CHANNEL_SET_TIMEOUT_EX:
	{
		u32 timeout =
			(u32)((struct nvhost_set_timeout_args *)buf)->timeout;
		bool timeout_debug_dump = !((u32)
			((struct nvhost_set_timeout_ex_args *)buf)->flags &
			(1 << NVHOST_TIMEOUT_FLAG_DISABLE_DUMP));
		priv->timeout = timeout;
		priv->timeout_debug_dump = timeout_debug_dump;
		dev_dbg(&priv->pdev->dev,
			"%s: setting buffer timeout (%d ms) for userctx 0x%p\n",
			__func__, priv->timeout, priv);
		break;
	}
	default:
		nvhost_dbg_info("unrecognized ioctl cmd: 0x%x", cmd);
		err = -ENOTTY;
		break;
	}

	mutex_unlock(&priv->ioctl_lock);

	if ((err == 0) && (_IOC_DIR(cmd) & _IOC_READ))
		err = copy_to_user((void __user *)arg, buf, _IOC_SIZE(cmd));

	return err;
}

static const struct file_operations nvhost_channelops = {
	.owner = THIS_MODULE,
	.release = nvhost_channelrelease,
	.open = nvhost_channelopen,
#ifdef CONFIG_COMPAT
	.compat_ioctl = nvhost_channelctl,
#endif
	.unlocked_ioctl = nvhost_channelctl
};

static const char *get_device_name_for_dev(struct platform_device *dev)
{
	struct nvhost_device_data *pdata = nvhost_get_devdata(dev);

	if (pdata->devfs_name)
		return pdata->devfs_name;

	return dev->name;
}

static struct device *nvhost_client_device_create(
	struct platform_device *pdev, struct cdev *cdev,
	const char *cdev_name, dev_t devno,
	const struct file_operations *ops)
{
	struct nvhost_master *host = nvhost_get_host(pdev);
	const char *use_dev_name;
	struct device *dev;
	int err;

	nvhost_dbg_fn("");

	BUG_ON(!host);

	cdev_init(cdev, ops);
	cdev->owner = THIS_MODULE;

	err = cdev_add(cdev, devno, 1);
	if (err < 0) {
		dev_err(&pdev->dev,
			"failed to add cdev\n");
		return NULL;
	}
	use_dev_name = get_device_name_for_dev(pdev);

	dev = device_create(host->nvhost_class,
			NULL, devno, NULL,
			(pdev->id <= 0) ?
			IFACE_NAME "-%s%s" :
			IFACE_NAME "-%s%s.%d",
			cdev_name, use_dev_name, pdev->id);

	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		dev_err(&pdev->dev,
			"failed to create %s %s device for %s\n",
			use_dev_name, cdev_name, pdev->name);
		return NULL;
	}

	return dev;
}

#define NVHOST_NUM_CDEV 4
int nvhost_client_user_init(struct platform_device *dev)
{
	dev_t devno;
	int err;
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);

	/* reserve 3 minor #s for <dev>, and ctrl-<dev> */

	err = alloc_chrdev_region(&devno, 0, NVHOST_NUM_CDEV, IFACE_NAME);
	if (err < 0) {
		dev_err(&dev->dev, "failed to allocate devno\n");
		goto fail;
	}
	pdata->cdev_region = devno;

	pdata->node = nvhost_client_device_create(dev, &pdata->cdev,
				"", devno, &nvhost_channelops);
	if (pdata->node == NULL)
		goto fail;

	/* module control (npn-channel based, global) interface */
	if (pdata->ctrl_ops) {
		++devno;
		pdata->ctrl_node = nvhost_client_device_create(dev,
					&pdata->ctrl_cdev, "ctrl-",
					devno, pdata->ctrl_ops);
		if (pdata->ctrl_node == NULL)
			goto fail;
	}

	return 0;
fail:
	return err;
}

static void nvhost_client_user_deinit(struct platform_device *dev)
{
	struct nvhost_master *nvhost_master = nvhost_get_host(dev);
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);

	if (pdata->node) {
		device_destroy(nvhost_master->nvhost_class, pdata->cdev.dev);
		cdev_del(&pdata->cdev);
	}

	if (pdata->as_node) {
		device_destroy(nvhost_master->nvhost_class, pdata->as_cdev.dev);
		cdev_del(&pdata->as_cdev);
	}

	if (pdata->ctrl_node) {
		device_destroy(nvhost_master->nvhost_class,
			       pdata->ctrl_cdev.dev);
		cdev_del(&pdata->ctrl_cdev);
	}

	unregister_chrdev_region(pdata->cdev_region, NVHOST_NUM_CDEV);
}

int nvhost_client_device_init(struct platform_device *dev)
{
	int err;
	struct nvhost_master *nvhost_master = nvhost_get_host(dev);
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);

	pdata->channels = kzalloc(pdata->num_channels *
					sizeof(struct nvhost_channel *),
					GFP_KERNEL);

	/* Create debugfs directory for the device */
	nvhost_device_debug_init(dev);

	err = nvhost_client_user_init(dev);
	if (err)
		goto fail;

	err = nvhost_device_list_add(dev);
	if (err)
		goto fail;

	if (pdata->scaling_init)
		pdata->scaling_init(dev);

	/* reset syncpoint values for this unit */
	err = nvhost_module_busy(nvhost_master->dev);
	if (err)
		goto fail_busy;

	nvhost_module_idle(nvhost_master->dev);

	/* Initialize dma parameters */
	dev->dev.dma_parms = &pdata->dma_parms;
	dma_set_max_seg_size(&dev->dev, UINT_MAX);

	dev_info(&dev->dev, "initialized\n");

	if (pdata->slave && !pdata->slave_initialized) {
		struct nvhost_device_data *slave_pdata =
					pdata->slave->dev.platform_data;
		slave_pdata->master = dev;
		pdata->slave->dev.parent = dev->dev.parent;
		platform_device_register(pdata->slave);
		pdata->slave_initialized = 1;
	}

	if (pdata->hw_init)
		return pdata->hw_init(dev);

	return 0;

fail_busy:
	/* Remove from nvhost device list */
	nvhost_device_list_remove(dev);
fail:
	/* Add clean-up */
	dev_err(&dev->dev, "failed to init client device\n");
	nvhost_client_user_deinit(dev);
	nvhost_device_debug_deinit(dev);
	return err;
}
EXPORT_SYMBOL(nvhost_client_device_init);

int nvhost_client_device_release(struct platform_device *dev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);

	/* Release nvhost module resources */
	nvhost_module_deinit(dev);

	/* Remove from nvhost device list */
	nvhost_device_list_remove(dev);

	/* Release chardev and device node for user space */
	nvhost_client_user_deinit(dev);

	/* Remove debugFS */
	nvhost_device_debug_deinit(dev);

	/* Release all nvhost channel of dev*/
	nvhost_channel_release(pdata);

	return 0;
}
EXPORT_SYMBOL(nvhost_client_device_release);

int nvhost_client_device_get_resources(struct platform_device *dev)
{
	int i;
	void __iomem *regs = NULL;
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);
	int ret;

	for (i = 0; i < dev->num_resources; i++) {
		struct resource *r = NULL;

		r = platform_get_resource(dev, IORESOURCE_MEM, i);
		/* We've run out of mem resources */
		if (!r)
			break;

		regs = devm_ioremap_resource(&dev->dev, r);
		if (IS_ERR(regs)) {
			ret = PTR_ERR(regs);
			goto fail;
		}

		pdata->aperture[i] = regs;
	}

	return 0;

fail:
	dev_err(&dev->dev, "failed to get register memory\n");

	return ret;
}
EXPORT_SYMBOL(nvhost_client_device_get_resources);

/* This is a simple wrapper around request_firmware that takes
 * 'fw_name' and if available applies a SOC relative path prefix to it.
 * The caller is responsible for calling release_firmware later.
 */
const struct firmware *
nvhost_client_request_firmware(struct platform_device *dev, const char *fw_name)
{
	struct nvhost_chip_support *op = nvhost_get_chip_ops();
	const struct firmware *fw;
	char *fw_path = NULL;
	int path_len, err;

	/* This field is NULL when calling from SYS_EXIT.
	   Add a check here to prevent crash in request_firmware */
	if (!current->fs) {
		BUG();
		return NULL;
	}

	if (!fw_name)
		return NULL;

	if (op->soc_name) {
		path_len = strlen(fw_name) + strlen(op->soc_name);
		path_len += 2; /* for the path separator and zero terminator*/

		fw_path = kzalloc(sizeof(*fw_path) * path_len,
				     GFP_KERNEL);
		if (!fw_path)
			return NULL;

		sprintf(fw_path, "%s/%s", op->soc_name, fw_name);
		fw_name = fw_path;
	}

	err = request_firmware(&fw, fw_name, &dev->dev);
	kfree(fw_path);
	if (err) {
		dev_err(&dev->dev, "failed to get firmware\n");
		return NULL;
	}

	/* note: caller must release_firmware */
	return fw;
}
EXPORT_SYMBOL(nvhost_client_request_firmware);
