/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Apple M5 GPU DRM driver: the drm/asahi UAPI (asahi_drm.h, as Aurora Mesa
 * uses it) over the kernel's apple_agx (sys/arm64/apple/apple_agx_api.h).
 *
 * First form: one VM and one compute queue for every client, the GPU VM
 * being apple_agx's client VM; GEM objects are physically contiguous; a
 * submission runs each compute command to retirement before the ioctl
 * returns, so its output syncs are signalled with an already-signalled
 * fence. Render commands are refused.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <vm/vm.h>
#include <machine/cpufunc.h>

#include <linux/module.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/pci.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_vma_manager.h>

#include <drm/asahi_drm.h>

#include <arm64/apple/apple_agx_api.h>

#define	AGX_PAGE		0x4000ul	/* UAT page = kernel page */
#define	AGX_VM_ID		1
#define	AGX_QUEUE_ID		1
#define	AGX_TIMESTAMP_HZ	24000000ul
#define	AGX_VM_KERNEL_MIN	0x20000000ul	/* as the M5 kernels report */

struct agx_drm {
	struct drm_device	drm;
	device_t		gpu;
	struct apple_agx_params	params;
	struct mutex		lock;		/* VM and queue */
	bool			vm_live;
	bool			queue_live;
};

struct agx_bo {
	struct drm_gem_object	base;
	struct page		*pages;
	vm_paddr_t		pa;
	size_t			size;
};

#define	to_agx(d)	container_of(d, struct agx_drm, drm)
#define	to_bo(o)	container_of(o, struct agx_bo, base)

/* GEM objects. */

static void
agx_bo_free(struct drm_gem_object *obj)
{
	struct agx_bo *bo = to_bo(obj);

	drm_gem_object_release(obj);
	if (bo->pages != NULL)
		__free_pages(bo->pages, get_order(bo->size));
	kfree(bo);
}

/*
 * LinuxKPI gives the VMA its VM object only after mmap returns, so the
 * pages go in at fault time (vmf_insert_pfn), as drm-kmod's TTM does.
 */
static vm_fault_t
agx_bo_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct agx_bo *bo = to_bo((struct drm_gem_object *)vma->vm_private_data);
	unsigned long off = vmf->address - vma->vm_start;

	if (off >= bo->size)
		return (VM_FAULT_SIGBUS);
	return (lkpi_vmf_insert_pfn_prot_locked(vma, vmf->address,
	    (bo->pa + off) >> PAGE_SHIFT, vma->vm_page_prot));
}

static int
agx_bo_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct agx_bo *bo = to_bo(obj);

	if (vma->vm_end - vma->vm_start > bo->size)
		return (-EINVAL);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	return (0);
}

static const struct vm_operations_struct agx_bo_vm_ops = {
	.fault = agx_bo_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct drm_gem_object_funcs agx_bo_funcs = {
	.free = agx_bo_free,
	.mmap = agx_bo_mmap,
	.vm_ops = &agx_bo_vm_ops,
};

static struct agx_bo *
agx_bo_create(struct drm_device *drm, size_t size)
{
	struct agx_bo *bo;

	size = roundup(size, AGX_PAGE);
	if (size == 0)
		return (ERR_PTR(-EINVAL));
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (bo == NULL)
		return (ERR_PTR(-ENOMEM));
	bo->size = size;
	bo->pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order(size));
	if (bo->pages == NULL) {
		kfree(bo);
		return (ERR_PTR(-ENOMEM));
	}
	bo->pa = page_to_phys(bo->pages);
	/* The GPU maps it uncached; push the zeroes out of the caches. */
	cpu_dcache_wbinv_range((void *)PHYS_TO_DMAP(bo->pa), size);
	bo->base.funcs = &agx_bo_funcs;
	drm_gem_private_object_init(drm, &bo->base, size);
	return (bo);
}

/* Ioctls. */

static int
agx_ioctl_get_params(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct drm_asahi_get_params *args = data;
	struct drm_asahi_params_global p;
	int i;

	if (args->param_group != 0 || args->pad != 0)
		return (-EINVAL);
	memset(&p, 0, sizeof(p));
	p.gpu_generation = agx->params.generation;
	p.gpu_variant = agx->params.variant;
	p.gpu_revision = agx->params.revision;
	p.chip_id = agx->params.chip_id;
	p.num_dies = agx->params.num_dies;
	p.num_clusters_total = agx->params.num_clusters;
	p.num_cores_per_cluster = agx->params.num_cores_per_cluster;
	for (i = 0; i < 3 && i < DRM_ASAHI_MAX_CLUSTERS; i++)
		p.core_masks[i] = agx->params.core_masks[i];
	p.vm_start = agx->params.vm_start;
	p.vm_end = agx->params.vm_end;
	p.vm_kernel_min_size = AGX_VM_KERNEL_MIN;
	p.max_commands_per_submission = 64;
	p.max_attachments = 16;
	p.command_timestamp_frequency_hz = AGX_TIMESTAMP_HZ;
	if (copy_to_user(u64_to_user_ptr(args->pointer), &p,
	    min_t(u64, args->size, sizeof(p))))
		return (-EFAULT);
	return (0);
}

static int
agx_ioctl_get_time(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_asahi_get_time *args = data;

	if (args->flags != 0)
		return (-EINVAL);
	args->gpu_timestamp = READ_SPECIALREG(cntpct_el0);
	return (0);
}

static int
agx_ioctl_vm_create(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct drm_asahi_vm_create *args = data;
	int ret = 0;

	mutex_lock(&agx->lock);
	if (agx->vm_live)
		ret = -ENOSPC;		/* one VM, as yet */
	else {
		agx->vm_live = true;
		args->vm_id = AGX_VM_ID;
	}
	mutex_unlock(&agx->lock);
	return (ret);
}

static int
agx_ioctl_vm_destroy(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct drm_asahi_vm_destroy *args = data;

	if (args->vm_id != AGX_VM_ID)
		return (-ENOENT);
	mutex_lock(&agx->lock);
	agx->vm_live = false;
	mutex_unlock(&agx->lock);
	return (0);
}

static int
agx_ioctl_gem_create(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_asahi_gem_create *args = data;
	struct agx_bo *bo;
	int ret;

	bo = agx_bo_create(drm, args->size);
	if (IS_ERR(bo))
		return (PTR_ERR(bo));
	ret = drm_gem_handle_create(file, &bo->base, &args->handle);
	drm_gem_object_put(&bo->base);
	return (ret);
}

static int
agx_ioctl_gem_mmap_offset(struct drm_device *drm, void *data,
    struct drm_file *file)
{
	struct drm_asahi_gem_mmap_offset *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (args->flags != 0)
		return (-EINVAL);
	obj = drm_gem_object_lookup(file, args->handle);
	if (obj == NULL)
		return (-ENOENT);
	ret = drm_gem_create_mmap_offset(obj);
	if (ret == 0)
		args->offset = drm_vma_node_offset_addr(&obj->vma_node);
	drm_gem_object_put(obj);
	return (ret);
}

static int
agx_bind_op(struct agx_drm *agx, struct drm_file *file,
    const struct drm_asahi_gem_bind_op *op)
{
	struct drm_gem_object *obj;
	struct agx_bo *bo;
	int ret;

	if ((op->addr | op->range | op->offset) & (AGX_PAGE - 1) ||
	    op->range == 0)
		return (-EINVAL);
	if (op->flags & DRM_ASAHI_BIND_UNBIND)
		return (apple_agx_vm_unmap(agx->gpu, op->addr, op->range) == 0 ?
		    0 : -EINVAL);
	if (op->flags & ~(DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE))
		return (-EOPNOTSUPP);	/* single-page and sparse binds */
	obj = drm_gem_object_lookup(file, op->handle);
	if (obj == NULL)
		return (-ENOENT);
	bo = to_bo(obj);
	ret = op->offset + op->range < op->offset ||
	    op->offset + op->range > bo->size ? -EINVAL :
	    apple_agx_vm_map(agx->gpu, op->addr, bo->pa + op->offset,
	    op->range, op->flags & DRM_ASAHI_BIND_WRITE) == 0 ? 0 : -EINVAL;
	drm_gem_object_put(obj);
	return (ret);
}

static int
agx_ioctl_vm_bind(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct drm_asahi_vm_bind *args = data;
	struct drm_asahi_gem_bind_op op;
	u32 i;
	int ret = 0;

	if (args->vm_id != AGX_VM_ID || args->stride < sizeof(op) ||
	    args->pad != 0)
		return (-EINVAL);
	for (i = 0; i < args->num_binds && ret == 0; i++) {
		if (copy_from_user(&op, u64_to_user_ptr(args->userptr +
		    (u64)i * args->stride), sizeof(op)))
			return (-EFAULT);
		ret = agx_bind_op(agx, file, &op);
	}
	return (ret);
}

static int
agx_ioctl_queue_create(struct drm_device *drm, void *data,
    struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct drm_asahi_queue_create *args = data;
	int ret = 0;

	if (args->vm_id != AGX_VM_ID ||
	    args->usc_exec_base != agx->params.usc_base)
		return (-EINVAL);
	mutex_lock(&agx->lock);
	if (agx->queue_live)
		ret = -EBUSY;		/* one queue, as yet */
	else {
		agx->queue_live = true;
		args->queue_id = AGX_QUEUE_ID;
	}
	mutex_unlock(&agx->lock);
	return (ret);
}

static int
agx_ioctl_queue_destroy(struct drm_device *drm, void *data,
    struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct drm_asahi_queue_destroy *args = data;

	if (args->queue_id != AGX_QUEUE_ID)
		return (-ENOENT);
	mutex_lock(&agx->lock);
	agx->queue_live = false;
	mutex_unlock(&agx->lock);
	return (0);
}

/* The input syncs' fences, waited for before the commands run. */
static int
agx_wait_in(struct drm_file *file, const struct drm_asahi_sync *s)
{
	struct dma_fence *fence;
	int ret;

	if (s->sync_type != DRM_ASAHI_SYNC_SYNCOBJ &&
	    s->sync_type != DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ)
		return (-EINVAL);
	ret = drm_syncobj_find_fence(file, s->handle,
	    s->sync_type == DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ ?
	    s->timeline_value : 0, 0, &fence);
	if (ret != 0)
		return (ret);
	ret = dma_fence_wait(fence, true);
	dma_fence_put(fence);
	return (ret < 0 ? ret : 0);
}

/* The commands have retired: signal an output sync. */
static int
agx_signal_out(struct drm_file *file, const struct drm_asahi_sync *s)
{
	struct drm_syncobj *syncobj;
	struct dma_fence_chain *chain;
	struct dma_fence *fence;

	syncobj = drm_syncobj_find(file, s->handle);
	if (syncobj == NULL)
		return (-ENOENT);
	fence = dma_fence_get_stub();
	if (s->sync_type == DRM_ASAHI_SYNC_SYNCOBJ)
		drm_syncobj_replace_fence(syncobj, fence);
	else if ((chain = dma_fence_chain_alloc()) != NULL)
		drm_syncobj_add_point(syncobj, chain, fence, s->timeline_value);
	dma_fence_put(fence);
	drm_syncobj_put(syncobj);
	return (0);
}

static int
agx_ioctl_submit(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct drm_asahi_submit *args = data;
	struct drm_asahi_sync *syncs = NULL;
	struct drm_asahi_cmd_header hdr;
	struct drm_asahi_cmd_compute cmd;
	u8 *buf = NULL;
	u32 n, off;
	int ret = 0;

	if (args->queue_id != AGX_QUEUE_ID || args->flags != 0 ||
	    args->cmdbuf_size > 0x10000)
		return (-EINVAL);
	n = args->in_sync_count + args->out_sync_count;
	if (n > 256)
		return (-EINVAL);
	if (n != 0) {
		syncs = kvmalloc_array(n, sizeof(*syncs), GFP_KERNEL);
		if (syncs == NULL)
			return (-ENOMEM);
		if (copy_from_user(syncs, u64_to_user_ptr(args->syncs),
		    n * sizeof(*syncs))) {
			ret = -EFAULT;
			goto out;
		}
	}
	buf = kvmalloc(args->cmdbuf_size, GFP_KERNEL);
	if (buf == NULL) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(buf, u64_to_user_ptr(args->cmdbuf),
	    args->cmdbuf_size)) {
		ret = -EFAULT;
		goto out;
	}
	for (n = 0; n < args->in_sync_count && ret == 0; n++)
		ret = agx_wait_in(file, &syncs[n]);
	for (off = 0; ret == 0 && off < args->cmdbuf_size; off += hdr.size) {
		if (args->cmdbuf_size - off < sizeof(hdr)) {
			ret = -EINVAL;
			break;
		}
		memcpy(&hdr, buf + off, sizeof(hdr));
		off += sizeof(hdr);
		if (hdr.size > args->cmdbuf_size - off) {
			ret = -EINVAL;
			break;
		}
		switch (hdr.cmd_type) {
		case DRM_ASAHI_CMD_COMPUTE:
			memset(&cmd, 0, sizeof(cmd));
			memcpy(&cmd, buf + off, min_t(u32, hdr.size,
			    sizeof(cmd)));
			if (cmd.sampler_count != 0) {
				ret = -EOPNOTSUPP;
				break;
			}
			mutex_lock(&agx->lock);
			ret = apple_agx_submit_compute(agx->gpu,
			    cmd.cdm_ctrl_stream_base, cmd.cdm_ctrl_stream_end,
			    0) == 0 ? 0 : -EIO;
			mutex_unlock(&agx->lock);
			break;
		case DRM_ASAHI_CMD_RENDER:
			ret = -EOPNOTSUPP;
			break;
		default:
			/* Attachment and resource commands: none needed yet. */
			break;
		}
	}
	for (n = 0; ret == 0 && n < args->out_sync_count; n++)
		ret = agx_signal_out(file, &syncs[args->in_sync_count + n]);
out:
	kvfree(buf);
	kvfree(syncs);
	return (ret);
}

#define	AGX_IOCTL(name, fn)	DRM_IOCTL_DEF_DRV(ASAHI_##name, fn, DRM_RENDER_ALLOW)

static const struct drm_ioctl_desc agx_ioctls[] = {
	AGX_IOCTL(GET_PARAMS, agx_ioctl_get_params),
	AGX_IOCTL(GET_TIME, agx_ioctl_get_time),
	AGX_IOCTL(VM_CREATE, agx_ioctl_vm_create),
	AGX_IOCTL(VM_DESTROY, agx_ioctl_vm_destroy),
	AGX_IOCTL(VM_BIND, agx_ioctl_vm_bind),
	AGX_IOCTL(GEM_CREATE, agx_ioctl_gem_create),
	AGX_IOCTL(GEM_MMAP_OFFSET, agx_ioctl_gem_mmap_offset),
	AGX_IOCTL(QUEUE_CREATE, agx_ioctl_queue_create),
	AGX_IOCTL(QUEUE_DESTROY, agx_ioctl_queue_destroy),
	AGX_IOCTL(SUBMIT, agx_ioctl_submit),
};

DEFINE_DRM_GEM_FOPS(agx_fops);

static const struct drm_driver agx_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ |
	    DRIVER_SYNCOBJ_TIMELINE,
	.ioctls = agx_ioctls,
	.num_ioctls = ARRAY_SIZE(agx_ioctls),
	.fops = &agx_fops,
	.name = "asahi",
	.desc = "Apple M5 GPU",
	.major = 1,
	.minor = 0,
};

/* Module glue: a LinuxKPI device over the newbus apple_agx device. */

/*
 * drm-kmod's drm_sysctl_init (drm_add_busid_modesetting) takes every DRM
 * device for a PCI one: to_pci_dev(dev->dev)->bus. Give it a zeroed
 * pci_dev with a bus to read, so the busid reads pci:0000:00:00.0.
 */
static struct pci_bus agx_pbus;
static struct pci_dev agx_pdev = { .bus = &agx_pbus };
#define	agx_ldev	(agx_pdev.dev)
static struct agx_drm *agx_dev;

static void
agx_ldev_release(struct device *dev)
{
}

static int __init
agxdrm_init(void)
{
	device_t gpu = apple_agx_device();
	struct agx_drm *agx;
	int ret;

	if (gpu == NULL) {
		printf("agxdrm: no running apple_agx\n");
		return (-ENXIO);
	}
	agx_ldev.release = agx_ldev_release;
	INIT_LIST_HEAD(&agx_ldev.irqents);
	spin_lock_init(&agx_ldev.devres_lock);
	INIT_LIST_HEAD(&agx_ldev.devres_head);
	mutex_init(&agx_ldev.lock);
	kobject_init(&agx_ldev.kobj, &linux_dev_ktype);
	kobject_set_name(&agx_ldev.kobj, "%s", device_get_nameunit(gpu));
	agx_ldev.bsddev = gpu;

	agx = devm_drm_dev_alloc(&agx_ldev, &agx_driver, struct agx_drm, drm);
	if (IS_ERR(agx))
		return (PTR_ERR(agx));
	agx->gpu = gpu;
	apple_agx_get_params(gpu, &agx->params);
	mutex_init(&agx->lock);
	ret = drm_dev_register(&agx->drm, 0);
	if (ret != 0)
		return (ret);
	agx_dev = agx;
	device_printf(gpu, "agxdrm: drm/asahi on card%d, renderD%d\n",
	    agx->drm.primary->index, agx->drm.render->index);
	return (0);
}

static void __exit
agxdrm_exit(void)
{

	if (agx_dev != NULL)
		drm_dev_unregister(&agx_dev->drm);
}

LKPI_DRIVER_MODULE(agxdrm, agxdrm_init, agxdrm_exit);
MODULE_DEPEND(agxdrm, drmn, 2, 2, 2);
MODULE_DEPEND(agxdrm, dmabuf, 1, 1, 1);
MODULE_DEPEND(agxdrm, linuxkpi, 1, 1, 1);
MODULE_DEPEND(agxdrm, linuxkpi_video, 1, 1, 1);
