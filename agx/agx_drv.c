/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Apple M5 GPU DRM driver: the drm/asahi UAPI (asahi_drm.h, as Aurora Mesa
 * uses it) over the kernel's apple_agx (sys/arm64/apple/apple_agx_api.h).
 *
 * Each DRM file has a hardware address space and ordered compute queue.
 * Each live queue leases one of the firmware compute channels.
 * GEM objects are arrays of pages. Render commands run on the GPU's
 * one render pair; the first few are also logged (agx_capture) for the
 * host reference tools.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <vm/vm.h>
#include <machine/cpufunc.h>

#include <linux/module.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
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
#include <arm64/apple/apple_agx_render.h>

#define	AGX_PAGE		0x4000ul	/* UAT page */
/* Each UAT page is one aligned block of kernel pages. */
#define	AGX_PAGE_ORDER		(ilog2(AGX_PAGE) - PAGE_SHIFT)
#define	AGX_MAX_COMMANDS	64
#define	AGX_MAX_SYNCS		256
#define	AGX_MAX_PENDING	64
#define	AGX_USC_GENERATION	3
#define	AGX_TIMESTAMP_HZ	1000000000ul
#define	AGX_VM_KERNEL_MIN	0x20000000ul	/* as the M5 kernels report */

struct agx_queue {
	struct list_head	link;
	struct workqueue_struct	*submit_wq;
	struct dma_fence	*last_render;	/* owned by submit_wq */
	atomic_t		pending;
	u64			fence_context;
	u64			fence_seqno;
	u32			id;
	u_int			slot;
	bool			cancel_jobs;
};

struct agx_vm {
	struct list_head	bindings;
	struct list_head	queues;
	struct list_head	ts_objects;	/* GEM_BIND_OBJECT timestamps */
	u32			next_ts_handle;
	wait_queue_head_t	idle;
	atomic_t		pending;
	u32			vm_id;
	u32			next_queue_id;
	u_int			context;
	int			error;
	bool			live;
	bool			quarantined;
};

struct agx_drm {
	struct drm_device	drm;
	device_t		gpu;
	struct apple_agx_params	params;
	struct mutex		lock;		/* VM binding and queue state */
	bool			gpu_failed;
};

/* Pages are allocated one at a time: large contiguous runs are not available. */
struct agx_bo {
	struct drm_gem_object	base;
	struct page		**pages;
	size_t			npages;
	size_t			size;
};

static vm_paddr_t
agx_bo_pa(const struct agx_bo *bo, size_t off)
{

	return (page_to_phys(bo->pages[off / AGX_PAGE]) + off % AGX_PAGE);
}

struct agx_binding {
	struct list_head	link;
	struct drm_gem_object	*obj;
	uint64_t		start;
	uint64_t		end;
	bool			closed;
};

#define	to_agx(d)	container_of(d, struct agx_drm, drm)
#define	to_bo(o)	container_of(o, struct agx_bo, base)

/*
 * A BO range bound as a timestamp buffer. The M5 firmware writes stamps
 * only inside its own timestamp aperture, so the driver publishes them from
 * the CPU when a command completes, as Aurora's G16 runtime does
 * (g16/poll.rs publish_timestamps): before the queue's next command runs
 * and before the job's fence signals.
 */
struct agx_ts_object {
	struct list_head	link;
	struct drm_gem_object	*obj;	/* referenced */
	u64			offset;
	u64			range;
	u32			handle;
};

static struct agx_ts_object *
agx_find_ts_object(struct agx_vm *vm, u32 handle)
{
	struct agx_ts_object *ts;

	list_for_each_entry(ts, &vm->ts_objects, link)
		if (ts->handle == handle)
			return (ts);
	return (NULL);
}

/* The GET_TIME timebase, in nanoseconds (AGX_TIMESTAMP_HZ). */
static u64
agx_now(void)
{
	uint64_t ticks, frequency;

	ticks = READ_SPECIALREG(cntpct_el0);
	frequency = READ_SPECIALREG(cntfrq_el0);
	return (ticks / frequency * AGX_TIMESTAMP_HZ +
	    ticks % frequency * AGX_TIMESTAMP_HZ / frequency);
}

static void
agx_free_ts_objects(struct agx_vm *vm)
{
	struct agx_ts_object *ts, *next;

	list_for_each_entry_safe(ts, next, &vm->ts_objects, link) {
		list_del(&ts->link);
		drm_gem_object_put(ts->obj);
		kfree(ts);
	}
}

/*
 * The firmware can follow arbitrary pointers from a command stream. Keep
 * the VM stable while it runs, including when a client closes a GEM handle.
 * The mutex serializes new submissions against the caller's eventual edit.
 */
static int
agx_wait_idle_locked(struct agx_drm *agx, struct agx_vm *vm,
    atomic_t *pending, bool interruptible)
{
	int ret = 0;

	while (atomic_read(pending) != 0 && ret == 0) {
		mutex_unlock(&agx->lock);
		if (interruptible)
			ret = wait_event_interruptible(vm->idle,
			    atomic_read(pending) == 0);
		else
			wait_event(vm->idle, atomic_read(pending) == 0);
		mutex_lock(&agx->lock);
	}
	return (ret);
}

/* GEM objects. */

static void
agx_free_binding(struct agx_binding *binding)
{
	list_del(&binding->link);
	drm_gem_object_put(binding->obj);
	kfree(binding);
}

static void
agx_drop_bindings(struct agx_drm *agx, struct agx_vm *vm, bool only_closed)
{
	struct agx_binding *binding, *next;

	if (vm->quarantined)
		return;
	list_for_each_entry_safe(binding, next, &vm->bindings, link) {
		if (only_closed && !binding->closed)
			continue;
		if (apple_agx_vm_unmap(agx->gpu, vm->context, binding->start,
		    binding->end - binding->start) != 0) {
			vm->quarantined = true;
			WRITE_ONCE(agx->gpu_failed, true);
			return;
		}
		agx_free_binding(binding);
	}
}

static int
agx_open(struct drm_device *drm, struct drm_file *file)
{
	struct agx_vm *vm;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (vm == NULL)
		return (-ENOMEM);
	INIT_LIST_HEAD(&vm->bindings);
	INIT_LIST_HEAD(&vm->queues);
	INIT_LIST_HEAD(&vm->ts_objects);
	init_waitqueue_head(&vm->idle);
	atomic_set(&vm->pending, 0);
	file->driver_priv = vm;
	return (0);
}

static void
agx_bo_close(struct drm_gem_object *obj, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(obj->dev);
	struct agx_vm *vm = file->driver_priv;
	struct agx_binding *binding;

	mutex_lock(&agx->lock);
	list_for_each_entry(binding, &vm->bindings, link)
		if (binding->obj == obj)
			binding->closed = true;
	if (atomic_read(&vm->pending) == 0)
		agx_drop_bindings(agx, vm, true);
	mutex_unlock(&agx->lock);
}

static void
agx_free_queue(struct agx_queue *queue)
{
	destroy_workqueue(queue->submit_wq);
	dma_fence_put(queue->last_render);
	kfree(queue);
}

static void
agx_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct agx_vm *vm = file->driver_priv;
	struct agx_queue *queue, *next;
	bool quarantined;

	mutex_lock(&agx->lock);
	list_for_each_entry(queue, &vm->queues, link)
		WRITE_ONCE(queue->cancel_jobs, true);
	agx_wait_idle_locked(agx, vm, &vm->pending, false);
	list_for_each_entry(queue, &vm->queues, link)
		apple_agx_queue_destroy(agx->gpu, queue->slot);
	agx_drop_bindings(agx, vm, false);
	agx_free_ts_objects(vm);
	quarantined = vm->quarantined;
	if (vm->context != 0 && !quarantined)
		apple_agx_vm_destroy(agx->gpu, vm->context);
	mutex_unlock(&agx->lock);

	list_for_each_entry_safe(queue, next, &vm->queues, link) {
		list_del(&queue->link);
		agx_free_queue(queue);
	}
	if (!quarantined)
		kfree(vm);
}

static void
agx_bo_free(struct drm_gem_object *obj)
{
	struct agx_bo *bo = to_bo(obj);
	size_t i;

	drm_gem_object_release(obj);
	if (bo->pages != NULL) {
		for (i = 0; i < bo->npages; i++)
			if (bo->pages[i] != NULL)
				__free_pages(bo->pages[i], AGX_PAGE_ORDER);
		kvfree(bo->pages);
	}
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
	    agx_bo_pa(bo, off) >> PAGE_SHIFT, vma->vm_page_prot));
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
	.close = agx_bo_close,
	.mmap = agx_bo_mmap,
	.vm_ops = &agx_bo_vm_ops,
};

static void
agx_bo_release_pages(struct agx_bo *bo)
{
	size_t i;

	for (i = 0; i < bo->npages; i++)
		if (bo->pages[i] != NULL)
			__free_pages(bo->pages[i], AGX_PAGE_ORDER);
	kvfree(bo->pages);
	kfree(bo);
}

static struct agx_bo *
agx_bo_create(struct drm_device *drm, size_t size)
{
	struct agx_bo *bo;
	size_t i;

	size = roundup(size, AGX_PAGE);
	if (size == 0)
		return (ERR_PTR(-EINVAL));
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (bo == NULL)
		return (ERR_PTR(-ENOMEM));
	bo->size = size;
	bo->npages = size / AGX_PAGE;
	bo->pages = kvcalloc(bo->npages, sizeof(*bo->pages), GFP_KERNEL);
	if (bo->pages == NULL) {
		kfree(bo);
		return (ERR_PTR(-ENOMEM));
	}
	for (i = 0; i < bo->npages; i++) {
		bo->pages[i] = alloc_pages(GFP_KERNEL | __GFP_ZERO,
		    AGX_PAGE_ORDER);
		if (bo->pages[i] == NULL) {
			agx_bo_release_pages(bo);
			return (ERR_PTR(-ENOMEM));
		}
		/* The GPU maps it uncached; push the zeroes out of the caches. */
		cpu_dcache_wbinv_range(
		    (void *)PHYS_TO_DMAP(page_to_phys(bo->pages[i])), AGX_PAGE);
	}
	bo->base.funcs = &agx_bo_funcs;
	drm_gem_private_object_init(drm, &bo->base, size);
	return (bo);
}

/* Ioctls. */

static int
agx_get_vm_status(struct agx_drm *agx, struct drm_file *file,
    const struct drm_asahi_get_params *args)
{
	struct agx_vm *vm = file->driver_priv;
	struct drm_asahi_vm_status status;
	void __user *user = u64_to_user_ptr(args->pointer);

	if (args->size != sizeof(status))
		return (-EINVAL);
	if (copy_from_user(&status, user, sizeof(status)))
		return (-EFAULT);
	if (status.flags != 0 || status.pad != 0)
		return (-EINVAL);
	mutex_lock(&agx->lock);
	if (!vm->live || status.vm_id != vm->vm_id) {
		mutex_unlock(&agx->lock);
		return (-ENOENT);
	}
	status.error = vm->error != 0 ? vm->error : agx->gpu_failed ? -EIO : 0;
	mutex_unlock(&agx->lock);
	return (copy_to_user(user, &status, sizeof(status)) ? -EFAULT : 0);
}

static int
agx_ioctl_get_params(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct drm_asahi_get_params *args = data;
	struct drm_asahi_params_global p;
	struct drm_asahi_queue_limits limits = {
		.max_queues = apple_agx_queue_count(agx->gpu),
		.max_in_flight_per_queue = 1,
	};
	int i;

	if (args->pad != 0)
		return (-EINVAL);
	if (args->param_group == DRM_ASAHI_PARAM_GROUP_VM_STATUS)
		return (agx_get_vm_status(agx, file, args));
	if (args->param_group == DRM_ASAHI_PARAM_GROUP_QUEUE_LIMITS) {
		if (args->size != sizeof(limits))
			return (-EINVAL);
		return (copy_to_user(u64_to_user_ptr(args->pointer), &limits,
		    sizeof(limits)) ? -EFAULT : 0);
	}
	if (args->param_group != 0)
		return (-EINVAL);
	memset(&p, 0, sizeof(p));
	p.features = DRM_ASAHI_FEATURE_VM_STATUS |
	    DRM_ASAHI_FEATURE_INDEPENDENT_QUEUES |
	    DRM_ASAHI_FEATURE_RENDER_RESOURCES |
	    DRM_ASAHI_FEATURE_RENDER_RESOURCES_V2 |
	    DRM_ASAHI_FEATURE_TASK_MESH_STORAGE |
	    DRM_ASAHI_FEATURE_COMPUTE_RESOURCES;
	p.gpu_generation = agx->params.generation;
	p.gpu_variant = agx->params.variant;
	p.gpu_revision = agx->params.revision;
	p.usc_generation = AGX_USC_GENERATION;
	p.gpu_hal_generation = DRM_ASAHI_GPU_HAL_300;
	p.chip_id = agx->params.chip_id;
	p.num_dies = agx->params.num_dies;
	p.num_clusters_total = agx->params.num_clusters;
	p.num_cores_per_cluster = agx->params.num_cores_per_cluster;
	for (i = 0; i < 3 && i < DRM_ASAHI_MAX_CLUSTERS; i++)
		p.core_masks[i] = agx->params.core_masks[i];
	p.vm_start = agx->params.vm_start;
	p.vm_end = agx->params.vm_end;
	p.vm_kernel_min_size = AGX_VM_KERNEL_MIN;
	p.max_commands_per_submission = AGX_MAX_COMMANDS;
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
	args->gpu_timestamp = agx_now();
	return (0);
}

/*
 * DRM_IOCTL_ASAHI_GEM_BIND_OBJECT: bind a BO range as a timestamp buffer.
 * Handles are per file and nonzero. A submit takes its own GEM reference for
 * each stamp, so unbinding or closing the BO while it runs is safe.
 */
static int
agx_ioctl_gem_bind_object(struct drm_device *drm, void *data,
    struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct agx_vm *vm = file->driver_priv;
	struct drm_asahi_gem_bind_object *args = data;
	struct agx_ts_object *ts, *next;
	struct drm_gem_object *obj;

	if (args->vm_id != 0 || args->pad != 0)
		return (-EINVAL);
	if (args->op == DRM_ASAHI_BIND_OBJECT_OP_UNBIND) {
		if (args->flags != 0 || args->handle != 0 ||
		    args->offset != 0 || args->range != 0)
			return (-EINVAL);
		mutex_lock(&agx->lock);
		list_for_each_entry_safe(ts, next, &vm->ts_objects, link) {
			if (ts->handle == args->object_handle) {
				list_del(&ts->link);
				mutex_unlock(&agx->lock);
				drm_gem_object_put(ts->obj);
				kfree(ts);
				return (0);
			}
		}
		mutex_unlock(&agx->lock);
		return (-ENOENT);
	}
	if (args->op != DRM_ASAHI_BIND_OBJECT_OP_BIND ||
	    args->flags != DRM_ASAHI_BIND_OBJECT_USAGE_TIMESTAMPS ||
	    args->object_handle != 0 || args->range == 0 ||
	    (args->offset & 7) != 0 || (args->range & 7) != 0)
		return (-EINVAL);
	obj = drm_gem_object_lookup(file, args->handle);
	if (obj == NULL)
		return (-ENOENT);
	if (args->offset > to_bo(obj)->size ||
	    args->range > to_bo(obj)->size - args->offset) {
		drm_gem_object_put(obj);
		return (-EINVAL);
	}
	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (ts == NULL) {
		drm_gem_object_put(obj);
		return (-ENOMEM);
	}
	ts->obj = obj;
	ts->offset = args->offset;
	ts->range = args->range;
	mutex_lock(&agx->lock);
	do {
		ts->handle = ++vm->next_ts_handle;
	} while (ts->handle == 0 || agx_find_ts_object(vm, ts->handle) != NULL);
	list_add_tail(&ts->link, &vm->ts_objects);
	args->object_handle = ts->handle;
	mutex_unlock(&agx->lock);
	return (0);
}

static int
agx_ioctl_vm_create(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct agx_vm *vm = file->driver_priv;
	struct drm_asahi_vm_create *args = data;
	int ret = 0;

	mutex_lock(&agx->lock);
	if (agx->gpu_failed) {
		ret = -EIO;
		goto out;
	}
	if (vm->live) {
		ret = -ENOSPC;
		goto out;
	}
	ret = -apple_agx_vm_create(agx->gpu, &vm->context);
	if (ret != 0)
		goto out;
	vm->live = true;
	vm->error = 0;
	if (++vm->vm_id == 0)
		vm->vm_id++;
	args->vm_id = vm->vm_id;
out:
	mutex_unlock(&agx->lock);
	return (ret);
}

static int
agx_ioctl_vm_destroy(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct agx_vm *vm = file->driver_priv;
	struct drm_asahi_vm_destroy *args = data;
	int ret = 0;

	mutex_lock(&agx->lock);
	if (!vm->live || args->vm_id != vm->vm_id)
		ret = -ENOENT;
	else if (!list_empty(&vm->queues) || atomic_read(&vm->pending) != 0)
		ret = -EBUSY;
	else {
		agx_drop_bindings(agx, vm, false);
		if (!vm->quarantined) {
			apple_agx_vm_destroy(agx->gpu, vm->context);
			vm->context = 0;
		}
		vm->live = false;
	}
	mutex_unlock(&agx->lock);
	return (ret);
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

/* At most one binding can contain both ends of a nonempty interval. */
static int
agx_alloc_split(struct agx_vm *vm, uint64_t start, uint64_t end,
    struct agx_binding **split)
{
	struct agx_binding *binding;

	*split = NULL;
	list_for_each_entry(binding, &vm->bindings, link) {
		if (binding->start < start && end < binding->end) {
			*split = kmalloc(sizeof(**split), GFP_KERNEL);
			return (*split == NULL ? -ENOMEM : 0);
		}
	}
	return (0);
}

static int
agx_remove_interval(struct agx_drm *agx, struct agx_vm *vm, uint64_t start,
    uint64_t end,
    struct agx_binding *split, bool unmap)
{
	struct agx_binding *binding, *next;
	uint64_t first, last;

	list_for_each_entry_safe(binding, next, &vm->bindings, link) {
		first = max(start, binding->start);
		last = min(end, binding->end);
		if (first >= last)
			continue;
		if (unmap && apple_agx_vm_unmap(agx->gpu, vm->context, first,
		    last - first) != 0) {
			vm->quarantined = true;
			WRITE_ONCE(agx->gpu_failed, true);
			return (-EIO);
		}
		if (first > binding->start && last < binding->end) {
			split->obj = binding->obj;
			drm_gem_object_get(split->obj);
			split->closed = binding->closed;
			split->start = last;
			split->end = binding->end;
			list_add(&split->link, &binding->link);
			binding->end = first;
		} else if (first > binding->start) {
			binding->end = first;
		} else if (last < binding->end) {
			binding->start = last;
		} else {
			agx_free_binding(binding);
		}
	}
	return (0);
}

/* Map range bytes of bo from offset at va, one page at a time. */
static int
agx_map_pages(struct agx_drm *agx, struct agx_vm *vm, uint64_t va,
    struct agx_bo *bo, uint64_t offset, uint64_t range, u_int flags)
{
	uint64_t off;
	int ret;

	if ((flags & APPLE_AGX_VM_SINGLE) != 0)
		return (-apple_agx_vm_map(agx->gpu, vm->context, va,
		    agx_bo_pa(bo, offset), range, flags));
	for (off = 0; off < range; off += AGX_PAGE) {
		ret = -apple_agx_vm_map(agx->gpu, vm->context, va + off,
		    agx_bo_pa(bo, offset + off), AGX_PAGE, flags);
		if (ret != 0) {
			if (off != 0)
				apple_agx_vm_unmap(agx->gpu, vm->context, va, off);
			return (ret);
		}
	}
	return (0);
}

static int
agx_bind_op(struct agx_drm *agx, struct agx_vm *vm, struct drm_file *file,
    const struct drm_asahi_gem_bind_op *op)
{
	struct agx_binding *binding, *split;
	struct drm_gem_object *obj;
	struct agx_bo *bo;
	uint64_t end, mapped_size;
	u_int flags;
	int ret;

	end = op->addr + op->range;
	if ((op->addr | op->range | op->offset) & (AGX_PAGE - 1) ||
	    !apple_agx_vm_valid(agx->gpu, op->addr, op->range))
		return (-EINVAL);
	if (op->flags == DRM_ASAHI_BIND_UNBIND) {
		ret = agx_alloc_split(vm, op->addr, end, &split);
		if (ret != 0)
			return (ret);
		ret = agx_remove_interval(agx, vm, op->addr, end, split, true);
		if (ret != 0)
			kfree(split);
		return (ret);
	}
	if (op->flags & ~(DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE |
	    DRM_ASAHI_BIND_SINGLE_PAGE))
		return (-EOPNOTSUPP);
	flags = 0;
	if (op->flags & DRM_ASAHI_BIND_READ)
		flags |= APPLE_AGX_VM_READ;
	if (op->flags & DRM_ASAHI_BIND_WRITE)
		flags |= APPLE_AGX_VM_WRITE;
	if (op->flags & DRM_ASAHI_BIND_SINGLE_PAGE)
		flags |= APPLE_AGX_VM_SINGLE;
	if ((flags & (APPLE_AGX_VM_READ | APPLE_AGX_VM_WRITE)) == 0)
		return (-EINVAL);
	mapped_size = (flags & APPLE_AGX_VM_SINGLE) != 0 ?
	    AGX_PAGE : op->range;

	obj = drm_gem_object_lookup(file, op->handle);
	if (obj == NULL)
		return (-ENOENT);
	bo = to_bo(obj);
	if (op->offset > bo->size || mapped_size > bo->size - op->offset) {
		ret = -EINVAL;
		goto out;
	}
	binding = kmalloc(sizeof(*binding), GFP_KERNEL);
	if (binding == NULL) {
		ret = -ENOMEM;
		goto out;
	}
	ret = agx_alloc_split(vm, op->addr, end, &split);
	if (ret != 0) {
		kfree(binding);
		goto out;
	}
	ret = agx_map_pages(agx, vm, op->addr, bo, op->offset, op->range,
	    flags);
	if (ret != 0) {
		kfree(split);
		kfree(binding);
		goto out;
	}
	agx_remove_interval(agx, vm, op->addr, end, split, false);
	binding->obj = obj;
	binding->start = op->addr;
	binding->end = end;
	binding->closed = false;
	list_add(&binding->link, &vm->bindings);
	return (0);	/* the new binding owns the lookup reference */
out:
	drm_gem_object_put(obj);
	return (ret);
}

static int
agx_ioctl_vm_bind(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct agx_vm *vm = file->driver_priv;
	struct drm_asahi_vm_bind *args = data;
	struct drm_asahi_gem_bind_op op;
	u32 i;
	int ret = 0;

	if (args->stride < sizeof(op) ||
	    args->pad != 0)
		return (-EINVAL);
	for (i = 0; i < args->num_binds && ret == 0; i++) {
		if (copy_from_user(&op, u64_to_user_ptr(args->userptr +
		    (u64)i * args->stride), sizeof(op)))
			return (-EFAULT);
		mutex_lock(&agx->lock);
		if (vm->live && args->vm_id == vm->vm_id)
			ret = agx_wait_idle_locked(agx, vm, &vm->pending, true);
		if (ret == 0) {
			if (!vm->live || args->vm_id != vm->vm_id)
				ret = -ENOENT;
			else if (vm->error != 0)
				ret = vm->error;
			else if (agx->gpu_failed)
				ret = -EIO;
			else
				ret = agx_bind_op(agx, vm, file, &op);
		}
		mutex_unlock(&agx->lock);
	}
	return (ret);
}

/* The device lock protects the file-local queue IDs and their leases. */
static struct agx_queue *
agx_find_queue(struct agx_vm *vm, u32 id)
{
	struct agx_queue *queue;

	list_for_each_entry(queue, &vm->queues, link)
		if (queue->id == id)
			return (queue);
	return (NULL);
}

static int
agx_ioctl_queue_create(struct drm_device *drm, void *data,
    struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct agx_vm *vm = file->driver_priv;
	struct drm_asahi_queue_create *args = data;
	struct agx_queue *queue;
	int ret;

	if (args->usc_exec_base != agx->params.usc_base)
		return (-EINVAL);
	queue = kzalloc(sizeof(*queue), GFP_KERNEL);
	if (queue == NULL)
		return (-ENOMEM);
	queue->submit_wq = alloc_ordered_workqueue("agx-submit", WQ_MEM_RECLAIM);
	if (queue->submit_wq == NULL) {
		kfree(queue);
		return (-ENOMEM);
	}
	atomic_set(&queue->pending, 0);
	queue->fence_context = dma_fence_context_alloc(1);

	mutex_lock(&agx->lock);
	if (!vm->live || args->vm_id != vm->vm_id)
		ret = -ENOENT;
	else if (vm->error != 0 || agx->gpu_failed)
		ret = vm->error != 0 ? vm->error : -EIO;
	else
		ret = -apple_agx_queue_create(agx->gpu, &queue->slot);
	if (ret == 0) {
		do {
			queue->id = ++vm->next_queue_id;
		} while (queue->id == 0 || agx_find_queue(vm, queue->id) != NULL);
		list_add_tail(&queue->link, &vm->queues);
		args->queue_id = queue->id;
	}
	mutex_unlock(&agx->lock);
	if (ret != 0)
		agx_free_queue(queue);
	return (ret);
}

static int
agx_ioctl_queue_destroy(struct drm_device *drm, void *data,
    struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct agx_vm *vm = file->driver_priv;
	struct drm_asahi_queue_destroy *args = data;
	struct agx_queue *queue;

	mutex_lock(&agx->lock);
	queue = agx_find_queue(vm, args->queue_id);
	if (queue == NULL || queue->cancel_jobs) {
		mutex_unlock(&agx->lock);
		return (-ENOENT);
	}
	WRITE_ONCE(queue->cancel_jobs, true);
	agx_wait_idle_locked(agx, vm, &queue->pending, false);
	apple_agx_queue_destroy(agx->gpu, queue->slot);
	list_del(&queue->link);
	mutex_unlock(&agx->lock);
	agx_free_queue(queue);
	return (0);
}

#define	AGX_MAX_PROGRAMS	16	/* linked render programs per render */

/* Start and end of each stage (render: vertex, fragment) and 8 end copies. */
#define	AGX_MAX_STAMPS		12

struct agx_stamp {
	struct drm_gem_object	*obj;	/* referenced */
	u64			 offset;	/* in the BO */
	bool			 end;
};

struct agx_command {
	bool			 render;
	u64			 start;		/* compute: the CDM stream */
	u64			 end;
	struct agx_render_pass	 pass;		/* render */
	u_int			 nprograms;
	struct agx_render_program programs[AGX_MAX_PROGRAMS];
	u_int			 nstamps;
	struct agx_stamp	 stamps[AGX_MAX_STAMPS];
	u64			 started;	/* agx_now() at publication */
};

struct agx_job {
	struct work_struct	 work;
	struct agx_drm		*agx;
	struct agx_vm		*vm;
	struct agx_queue	*queue;
	struct dma_fence	*done;
	struct dma_fence	*deps[AGX_MAX_SYNCS];
	u32			 n_deps;
	struct agx_command	 commands[AGX_MAX_COMMANDS];
	u32			 n_commands;
};

struct agx_output {
	struct drm_syncobj	*syncobj;
	struct dma_fence_chain	*chain;
	u64			 point;
};

/* Write a completed command's timestamps through the direct map. */
static void
agx_publish_stamps(struct agx_command *c, u64 now)
{
	struct agx_stamp *s;
	u_int i;

	for (i = 0; i < c->nstamps; i++) {
		s = &c->stamps[i];
		atomic_store_rel_64((volatile uint64_t *)PHYS_TO_DMAP(
		    agx_bo_pa(to_bo(s->obj), s->offset)),
		    s->end ? now : c->started);
	}
	wmb();
}

static void
agx_job_free(struct agx_job *job)
{
	u32 i, j;

	for (i = 0; i < job->n_commands; i++)
		for (j = 0; j < job->commands[i].nstamps; j++)
			drm_gem_object_put(job->commands[i].stamps[j].obj);
	for (i = 0; i < job->n_deps; i++)
		dma_fence_put(job->deps[i]);
	dma_fence_put(job->done);
	kfree(job);
}

static void
agx_record_job_error(struct agx_drm *agx, struct agx_vm *vm, int error,
    bool gpu_failed)
{
	mutex_lock(&agx->lock);
	if (vm->error == 0)
		vm->error = error;
	/*
	 * A failing kick may still access memory. Keep the VM mappings
	 * and their GEM references until a GPU reset is available.
	 */
	if (gpu_failed) {
		vm->quarantined = true;
		WRITE_ONCE(agx->gpu_failed, true);
	}
	mutex_unlock(&agx->lock);
}

static void agx_job_finish(struct agx_job *job, int ret, bool gpu_failed);
static void agx_job_render_done(void *arg, int error);

static void
agx_run_job(struct work_struct *work)
{
	struct agx_job *job = container_of(work, struct agx_job, work);
	struct agx_drm *agx = job->agx;
	struct agx_vm *vm = job->vm;
	struct agx_queue *queue = job->queue;
	long waited;
	u32 i;
	int ret;
	bool gpu_failed = false;

	mutex_lock(&agx->lock);
	ret = vm->error;
	mutex_unlock(&agx->lock);
	for (i = 0; i < job->n_deps && ret == 0; i++) {
		/*
		 * Fence waits use bounded sleeps so that queue teardown can
		 * cancel them. A live queue has no dependency deadline.
		 */
		waited = -ECANCELED;
		do {
			if (READ_ONCE(queue->cancel_jobs))
				break;
			waited = dma_fence_wait_timeout(job->deps[i], false,
			    msecs_to_jiffies(100));
		} while (waited == 0);
		if (READ_ONCE(queue->cancel_jobs))
			ret = -ECANCELED;
		else if (waited < 0)
			ret = waited;
		else
			ret = min(0, dma_fence_get_status(job->deps[i]));
	}
	mutex_lock(&agx->lock);
	if (ret == 0) {
		if (queue->cancel_jobs)
			ret = -ECANCELED;
		else if (vm->error != 0)
			ret = vm->error;
		else if (agx->gpu_failed)
			ret = -EIO;
	}
	mutex_unlock(&agx->lock);
	for (i = 0; i < job->n_commands && ret == 0; i++) {
		struct agx_command *c = &job->commands[i];

		/*
		 * The job's last render returns once published: its
		 * completion finishes the job, so the queue's next job can
		 * publish behind it.
		 */
		if (c->render && i == job->n_commands - 1) {
			struct dma_fence *fence = dma_fence_get(job->done);

			c->started = agx_now();
			ret = -apple_agx_submit_render_async(agx->gpu,
			    vm->context, &c->pass, c->programs, c->nprograms,
			    agx_job_render_done, job);
			if (ret == 0) {
				dma_fence_put(queue->last_render);
				queue->last_render = fence;
				return;
			}
			dma_fence_put(fence);
			gpu_failed = ret != -EINVAL && ret != -E2BIG &&
			    ret != -EOPNOTSUPP;
			break;
		}
		/* The compute ring has no dependency on the render pair. */
		if (!c->render && queue->last_render != NULL) {
			ret = dma_fence_wait(queue->last_render, false);
			if (ret == 0)
				ret = min(0, dma_fence_get_status(queue->last_render));
			dma_fence_put(queue->last_render);
			queue->last_render = NULL;
			if (ret != 0)
				break;
		}
		c->started = agx_now();
		if (c->render)
			ret = -apple_agx_submit_render(agx->gpu, vm->context,
			    &c->pass, c->programs, c->nprograms, 0);
		else
			ret = -apple_agx_submit_compute(agx->gpu, queue->slot,
			    vm->context, c->start, c->end, c->programs,
			    c->nprograms, 0);
		gpu_failed = ret != 0 && ret != -EINVAL && ret != -E2BIG &&
		    ret != -EOPNOTSUPP;
		if (ret == 0)
			agx_publish_stamps(c, agx_now());
	}
	agx_job_finish(job, ret, gpu_failed);
}

/* A job's last render retired or failed (apple_agx task thread). */
static void
agx_job_render_done(void *arg, int error)
{
	struct agx_job *job = arg;

	if (error == 0)
		agx_publish_stamps(&job->commands[job->n_commands - 1],
		    agx_now());
	/*
	 * ECANCELED: a firmware recovery dropped this render and the GPU
	 * resumed. The VM fails with EIO; the device stays usable.
	 */
	if (error == ECANCELED) {
		agx_job_finish(job, -EIO, false);
		return;
	}
	agx_job_finish(job, -error, error != 0);
}

static void
agx_job_finish(struct agx_job *job, int ret, bool gpu_failed)
{
	struct agx_drm *agx = job->agx;
	struct agx_vm *vm = job->vm;
	struct agx_queue *queue = job->queue;
	bool queue_idle;

	if (ret != 0) {
		if (ret != -ECANCELED || gpu_failed)
			agx_record_job_error(agx, vm, ret, gpu_failed);
		dma_fence_set_error(job->done, ret);
	}
	dma_fence_signal(job->done);
	agx_job_free(job);

	mutex_lock(&agx->lock);
	queue_idle = atomic_dec_and_test(&queue->pending);
	if (atomic_dec_and_test(&vm->pending))
		agx_drop_bindings(agx, vm, true);
	if (queue_idle)
		wake_up_all(&vm->idle);
	mutex_unlock(&agx->lock);
}

/*
 * Log one submission's render-side command, header and payload, as hex
 * words for tools/agx_graph. Each line names the submission sequence, the
 * command index and the payload offset.
 */
static void
agx_capture(u_int seq, u_int index, const struct drm_asahi_cmd_header *hdr,
    const u8 *payload)
{
	char line[8 * 9 + 1];
	u32 off, word, i, n;

	printf("agxdrm: render cap %u.%u type %u size %u vdm %u cdm %u\n", seq,
	    index, hdr->cmd_type, hdr->size, hdr->vdm_barrier, hdr->cdm_barrier);
	for (off = 0; off < hdr->size; off += 32) {
		n = 0;
		for (i = off; i < off + 32 && i < hdr->size; i += 4) {
			word = 0;
			memcpy(&word, payload + i, min_t(u32, 4, hdr->size - i));
			n += snprintf(line + n, sizeof(line) - n, " %08x", word);
		}
		printf("agxdrm: render cap %u.%u +%04x:%s\n", seq, index, off,
		    line);
	}
}

/* The render flags command.rs admits on M5 that this driver advertises. */
#define	AGX_RENDER_FLAGS	(DRM_ASAHI_RENDER_PROCESS_EMPTY_TILES | \
	DRM_ASAHI_RENDER_RSRC_SPEC_HI | DRM_ASAHI_RENDER_DBIAS_IS_INT | \
	DRM_ASAHI_RENDER_SHADER_WRITES)

static bool
agx_helper_empty(const struct drm_asahi_helper_program *h)
{
	const u8 *b = (const u8 *)h;
	size_t i;

	for (i = 0; i < sizeof(*h); i++)
		if (b[i] != 0)
			return (false);
	return (true);
}

/* command.rs Render validation and RenderPass mapping, M5 subset. */
static int
agx_parse_render(const struct drm_asahi_cmd_render *c, struct agx_render_pass *p)
{
	u32 samples_log2;

	if ((c->flags & ~AGX_RENDER_FLAGS) != 0 ||
	    c->width_px < 1 || c->width_px > 16384 ||
	    c->height_px < 1 || c->height_px > 16384 ||
	    c->layers < 1 || c->layers > 2048 ||
	    !((c->utile_width_px == 16 && c->utile_height_px == 16) ||
	    (c->utile_width_px == 32 && c->utile_height_px == 16) ||
	    (c->utile_width_px == 32 && c->utile_height_px == 32)))
		return (-EINVAL);
	switch (c->samples) {
	case 1: samples_log2 = 0; break;
	case 2: samples_log2 = 1; break;
	case 4: samples_log2 = 2; break;
	case 8: samples_log2 = 3; break;
	default: return (-EINVAL);
	}
	if ((u64)c->sample_size_B * c->utile_width_px * c->utile_height_px *
	    c->samples > 65536 ||
	    c->vdm_ctrl_stream_base == 0 || (c->vdm_ctrl_stream_base & 3) != 0 ||
	    c->isp_scissor_base == 0 || (c->isp_scissor_base & 7) != 0 ||
	    (c->isp_dbias_base & 7) != 0 || (c->isp_oclqry_base & 7) != 0 ||
	    !agx_helper_empty(&c->vertex_helper) ||
	    !agx_helper_empty(&c->fragment_helper))
		return (-EINVAL);
	memset(p, 0, sizeof(*p));
	p->width = c->width_px;
	p->height = c->height_px;
	p->layers = c->layers;
	p->utile_width = c->utile_width_px;
	p->utile_height = c->utile_height_px;
	p->samples_log2 = samples_log2;
	p->sample_size = c->sample_size_B;
	p->process_empty_tiles =
	    (c->flags & DRM_ASAHI_RENDER_PROCESS_EMPTY_TILES) != 0;
	p->msaa_control = c->ppp_multisamplectl;
	p->ppp_control = c->ppp_ctrl;
	p->vdm_va = c->vdm_ctrl_stream_base;
	p->scissor_va = c->isp_scissor_base;
	p->depth_bias_va = c->isp_dbias_base;
	p->depth_bias_is_int = (c->flags & DRM_ASAHI_RENDER_DBIAS_IS_INT) != 0;
	p->shader_writes_memory = (c->flags & DRM_ASAHI_RENDER_SHADER_WRITES) != 0;
	p->occlusion_query_va = c->isp_oclqry_base;
	p->sampler_heap_va = c->sampler_heap;
	p->sampler_count = c->sampler_count;
	p->zls_control = c->zls_ctrl;
	p->depth_dimensions = c->isp_zls_pixels;
	p->depth = (struct agx_zls_surface){ c->depth.base, c->depth.comp_base,
	    c->depth.stride, c->depth.comp_stride };
	p->stencil = (struct agx_zls_surface){ c->stencil.base,
	    c->stencil.comp_base, c->stencil.stride, c->stencil.comp_stride };
	p->depth_clear = c->isp_bgobjdepth;
	p->stencil_clear = c->isp_bgobjvals;
	p->merge_upper_x = c->isp_merge_upper_x;
	p->merge_upper_y = c->isp_merge_upper_y;
	p->bg_offset = c->bg.usc;
	p->bg_rsrc_spec = c->bg.rsrc_spec | (u64)c->bg_rsrc_spec_hi << 32;
	p->partial_bg_offset = c->partial_bg.usc;
	p->partial_bg_rsrc_spec = c->partial_bg.rsrc_spec |
	    (u64)c->bg_partial_rsrc_spec_hi << 32;
	p->eot_offset = c->eot.usc;
	p->eot_rsrc_spec = c->eot.rsrc_spec | (u64)c->bg_eot_rsrc_spec_hi << 32;
	p->partial_eot_offset = c->partial_eot.usc;
	p->partial_eot_rsrc_spec = c->partial_eot.rsrc_spec |
	    (u64)c->bg_eot_partial_rsrc_spec_hi << 32;
	p->rsrc_spec_hi = (c->flags & DRM_ASAHI_RENDER_RSRC_SPEC_HI) != 0;
	return (agx_m5_render_validate(p) != 0 ? -EOPNOTSUPP : 0);
}

/*
 * render_resources.rs parse_v2, for the programs this driver runs: SCS
 * (ray-query scratch) is refused for now. Mesh, object and API task
 * programs run on the tiling engine with their thread count.
 */
static int
agx_parse_programs(const u8 *b, u32 size, struct agx_render_program *out,
    u_int *n)
{
	struct drm_asahi_render_program_v2 e;
	bool mesh;
	u32 off;

	if (size == 0 || size % sizeof(e) != 0)
		return (-EINVAL);
	if (size / sizeof(e) > AGX_MAX_PROGRAMS)
		return (-EOPNOTSUPP);
	*n = 0;
	for (off = 0; off < size; off += sizeof(e)) {
		memcpy(&e, b + off, sizeof(e));
		mesh = (e.usage_flags & (DRM_ASAHI_RENDER_PROGRAM_MESH |
		    DRM_ASAHI_RENDER_PROGRAM_OBJECT)) != 0;
		if (e.reserved != 0 || e.legacy.flags != 0 || e.legacy.stage > 1 ||
		    (e.usage_flags & ~0xfu) != 0 ||
		    (e.usage_flags & 6) == 6 || (mesh != (e.threads != 0)) ||
		    ((e.usage_flags & DRM_ASAHI_RENDER_PROGRAM_TASK) != 0 &&
		    (e.usage_flags & DRM_ASAHI_RENDER_PROGRAM_OBJECT) == 0) ||
		    (mesh && (e.legacy.stage != 0 ||
		    e.legacy.auxiliary_count != 0 ||
		    e.legacy.imageblock_bytes != 0 ||
		    e.legacy.multisample_bytes != 0)))
			return (-EINVAL);
		if (e.scs_layout != 0 || e.intersection_pool != 0 ||
		    (e.usage_flags & DRM_ASAHI_RENDER_PROGRAM_SCS) != 0 ||
		    e.auxiliary_allocation != 0)
			return (-EOPNOTSUPP);
		out[(*n)++] = (struct agx_render_program){
		    .stage = e.legacy.stage,
		    .primary_bytes = e.legacy.primary_bytes,
		    .secondary_bytes = e.legacy.secondary_bytes,
		    .linked_bytes = e.legacy.linked_bytes,
		    .minimum_primary_bytes = e.legacy.minimum_primary_bytes,
		    .auxiliary_count = e.legacy.auxiliary_count,
		    .threadgroup_bytes = e.legacy.threadgroup_bytes,
		    .imageblock_bytes = e.legacy.imageblock_bytes,
		    .multisample_bytes = e.legacy.multisample_bytes,
		    .threads = e.threads,
		    .task = (e.usage_flags & DRM_ASAHI_RENDER_PROGRAM_TASK) != 0,
		};
	}
	return (0);
}

/*
 * render_resources.rs ComputePrograms::parse: the storage of each distinct
 * dispatch in the following compute commands. An empty list clears it.
 */
static int
agx_parse_dispatches(const u8 *b, u32 size, struct agx_render_program *out,
    u_int *n)
{
	struct drm_asahi_compute_program e;
	u32 off;

	if (size % sizeof(e) != 0)
		return (-EINVAL);
	if (size / sizeof(e) > AGX_MAX_PROGRAMS)
		return (-EOPNOTSUPP);
	*n = 0;
	for (off = 0; off < size; off += sizeof(e)) {
		memcpy(&e, b + off, sizeof(e));
		if (e.flags != 0 || e.threads == 0)
			return (-EINVAL);
		out[(*n)++] = (struct agx_render_program){
		    .primary_bytes = e.primary_bytes,
		    .secondary_bytes = e.secondary_bytes,
		    .linked_bytes = e.linked_bytes,
		    .minimum_primary_bytes = e.minimum_primary_bytes,
		    .threadgroup_bytes = e.threadgroup_bytes,
		    .threads = e.threads,
		};
	}
	return (0);
}

/*
 * Resolve one drm_asahi_timestamp of a parsed command (already counted in
 * job->n_commands, so agx_job_free releases what this takes).
 */
static int
agx_add_stamp(struct agx_job *job, struct agx_command *c,
    const struct drm_asahi_timestamp *t, bool end)
{
	struct agx_ts_object *ts;
	int ret = 0;

	if (t->handle == 0)
		return (t->offset == 0 ? 0 : -EINVAL);
	if (c->nstamps >= AGX_MAX_STAMPS)
		return (-EINVAL);
	mutex_lock(&job->agx->lock);
	ts = agx_find_ts_object(job->vm, t->handle);
	if (ts == NULL)
		ret = -ENOENT;
	else if ((t->offset & 7) != 0 || t->offset + 8ul > ts->range)
		ret = -EINVAL;
	else {
		drm_gem_object_get(ts->obj);
		c->stamps[c->nstamps++] = (struct agx_stamp){
		    .obj = ts->obj,
		    .offset = ts->offset + t->offset,
		    .end = end,
		};
	}
	mutex_unlock(&job->agx->lock);
	return (ret);
}

/* A stage's start and end stamps, then (if any) the eight end copies. */
static int
agx_add_stamps(struct agx_job *job, struct agx_command *c,
    const struct drm_asahi_timestamps *stage,
    const struct drm_asahi_timestamp *copies)
{
	u_int i;
	int ret;

	if ((ret = agx_add_stamp(job, c, &stage->start, false)) != 0 ||
	    (ret = agx_add_stamp(job, c, &stage->end, true)) != 0)
		return (ret);
	for (i = 0; copies != NULL && i < 8; i++)
		if ((ret = agx_add_stamp(job, c, &copies[i], true)) != 0)
			return (ret);
	return (0);
}

static int
agx_parse_commands(struct agx_job *job, const u8 *buf, u32 size)
{
	static u_int capture_seq;
	struct drm_asahi_cmd_header hdr;
	struct drm_asahi_cmd_compute cmd;
	struct drm_asahi_cmd_render render;
	struct agx_render_program programs[AGX_MAX_PROGRAMS];
	struct agx_render_program dispatches[AGX_MAX_PROGRAMS];
	u_int nprograms = 0, ndispatches = 0, index, seq = 0;
	bool captured = false;
	u32 off;
	int ret;

	for (off = 0, index = 0; off < size; off += hdr.size, index++) {
		if (size - off < sizeof(hdr))
			return (-EINVAL);
		memcpy(&hdr, buf + off, sizeof(hdr));
		off += sizeof(hdr);
		if (hdr.size > size - off)
			return (-EINVAL);
		if (hdr.cmd_type != DRM_ASAHI_CMD_COMPUTE &&
		    hdr.cmd_type != DRM_ASAHI_SET_COMPUTE_ATTACHMENTS &&
		    capture_seq < 4) {
			if (!captured)
				seq = capture_seq++;
			captured = true;
			agx_capture(seq, index, &hdr, buf + off);
		}

		switch (hdr.cmd_type) {
		case DRM_ASAHI_CMD_COMPUTE:
			if (job->n_commands >= AGX_MAX_COMMANDS ||
			    hdr.size < offsetofend(struct drm_asahi_cmd_compute,
			    cdm_ctrl_stream_end))
				return (-EINVAL);
			memset(&cmd, 0, sizeof(cmd));
			memcpy(&cmd, buf + off, min_t(u32, hdr.size, sizeof(cmd)));
			if (cmd.sampler_count != 0)
				return (-EOPNOTSUPP);
			if ((cmd.cdm_ctrl_stream_base & 7) != 0 ||
			    cmd.cdm_ctrl_stream_end <= cmd.cdm_ctrl_stream_base ||
			    cmd.cdm_ctrl_stream_end - cmd.cdm_ctrl_stream_base < 4)
				return (-EINVAL);
			job->commands[job->n_commands++] = (struct agx_command){
			    .start = cmd.cdm_ctrl_stream_base,
			    .end = cmd.cdm_ctrl_stream_end,
			    .nprograms = ndispatches,
			};
			memcpy(job->commands[job->n_commands - 1].programs,
			    dispatches, ndispatches * sizeof(dispatches[0]));
			ret = agx_add_stamps(job,
			    &job->commands[job->n_commands - 1], &cmd.ts,
			    cmd.ts_end_copies);
			if (ret != 0)
				return (ret);
			break;
		case DRM_ASAHI_CMD_RENDER:
			if (job->n_commands >= AGX_MAX_COMMANDS ||
			    hdr.size < offsetof(struct drm_asahi_cmd_render,
			    bg_eot_rsrc_spec_hi))
				return (-EINVAL);
			memset(&render, 0, sizeof(render));
			memcpy(&render, buf + off, min_t(u32, hdr.size,
			    sizeof(render)));
			/* M5 renders carry their linked programs. */
			if (nprograms == 0)
				return (-EINVAL);
			job->commands[job->n_commands].render = true;
			job->commands[job->n_commands].nprograms = nprograms;
			memcpy(job->commands[job->n_commands].programs, programs,
			    nprograms * sizeof(programs[0]));
			ret = agx_parse_render(&render,
			    &job->commands[job->n_commands].pass);
			if (ret != 0) {
				printf("agxdrm: render refused: %d: %ux%u, %u "
				    "samples, %u B/sample, utile %ux%u, flags %#x, "
				    "zls %#jx, depth %#jx/%#jx, stencil %#jx/%#jx\n",
				    ret, render.width_px, render.height_px,
				    render.samples, render.sample_size_B,
				    render.utile_width_px, render.utile_height_px,
				    render.flags, (uintmax_t)render.zls_ctrl,
				    (uintmax_t)render.depth.base,
				    (uintmax_t)render.depth.comp_base,
				    (uintmax_t)render.stencil.base,
				    (uintmax_t)render.stencil.comp_base);
				return (ret);
			}
			job->n_commands++;
			ret = agx_add_stamps(job,
			    &job->commands[job->n_commands - 1], &render.ts_vtx,
			    NULL);
			if (ret == 0)
				ret = agx_add_stamps(job,
				    &job->commands[job->n_commands - 1],
				    &render.ts_frag, render.ts_frag_end_copies);
			if (ret != 0)
				return (ret);
			break;
		case DRM_ASAHI_SET_RENDER_RESOURCES_V2:
			if ((ret = agx_parse_programs(buf + off, hdr.size,
			    programs, &nprograms)) != 0) {
				printf("agxdrm: render programs refused: %d\n",
				    ret);
				return (ret);
			}
			break;
		case DRM_ASAHI_SET_COMPUTE_RESOURCES:
			if ((ret = agx_parse_dispatches(buf + off, hdr.size,
			    dispatches, &ndispatches)) != 0) {
				printf("agxdrm: compute programs refused: %d\n",
				    ret);
				return (ret);
			}
			break;
		case DRM_ASAHI_SET_COMPUTE_ATTACHMENTS:
		case DRM_ASAHI_SET_VERTEX_ATTACHMENTS:
		case DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS:
			/* Advisory: M5 kicks name no MCache table. */
			break;
		default:
			return (-EOPNOTSUPP);
		}
	}
	return (0);
}

static int
agx_prepare_syncs(struct drm_file *file, struct agx_job *job,
    struct agx_output *outputs, const struct drm_asahi_sync *syncs,
    u32 in_count, u32 out_count)
{
	const struct drm_asahi_sync *sync;
	u64 point;
	u32 i;
	int ret;

	for (i = 0; i < in_count + out_count; i++) {
		sync = &syncs[i];
		switch (sync->sync_type) {
		case DRM_ASAHI_SYNC_SYNCOBJ:
			point = 0;
			break;
		case DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ:
			point = sync->timeline_value;
			break;
		default:
			return (-EINVAL);
		}
		if (i < in_count) {
			ret = drm_syncobj_find_fence(file, sync->handle, point,
			    0, &job->deps[job->n_deps]);
			if (ret != 0)
				return (ret);
			job->n_deps++;
		} else {
			struct agx_output *out = &outputs[i - in_count];

			out->syncobj = drm_syncobj_find(file, sync->handle);
			if (out->syncobj == NULL)
				return (-ENOENT);
			if (sync->sync_type == DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ) {
				out->chain = dma_fence_chain_alloc();
				if (out->chain == NULL)
					return (-ENOMEM);
				out->point = point;
			}
		}
	}
	return (0);
}

static int
agx_ioctl_submit(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct agx_drm *agx = to_agx(drm);
	struct agx_vm *vm = file->driver_priv;
	struct drm_asahi_submit *args = data;
	struct drm_asahi_sync *syncs = NULL;
	struct agx_output *outputs = NULL;
	struct agx_job *job;
	struct agx_queue *queue;
	u8 *buf = NULL;
	u32 i, n;
	int ret = -ENOMEM;

	if (args->flags != 0 || args->pad != 0 ||
	    args->cmdbuf_size > 0x10000 || args->in_sync_count > AGX_MAX_SYNCS ||
	    args->out_sync_count > AGX_MAX_SYNCS - args->in_sync_count)
		return (-EINVAL);
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (job == NULL)
		return (-ENOMEM);
	job->agx = agx;
	job->vm = vm;
	INIT_WORK(&job->work, agx_run_job);

	n = args->in_sync_count + args->out_sync_count;
	if (n != 0) {
		syncs = kvmalloc_array(n, sizeof(*syncs), GFP_KERNEL);
		if (syncs == NULL)
			goto out;
		if (copy_from_user(syncs, u64_to_user_ptr(args->syncs),
		    n * sizeof(*syncs))) {
			ret = -EFAULT;
			goto out;
		}
	}
	if (args->out_sync_count != 0) {
		outputs = kcalloc(args->out_sync_count, sizeof(*outputs), GFP_KERNEL);
		if (outputs == NULL)
			goto out;
	}
	if (args->cmdbuf_size != 0) {
		buf = kvmalloc(args->cmdbuf_size, GFP_KERNEL);
		if (buf == NULL)
			goto out;
		if (copy_from_user(buf, u64_to_user_ptr(args->cmdbuf),
		    args->cmdbuf_size)) {
			ret = -EFAULT;
			goto out;
		}
	}
	ret = agx_parse_commands(job, buf, args->cmdbuf_size);
	if (ret != 0)
		goto out;
	ret = agx_prepare_syncs(file, job, outputs, syncs, args->in_sync_count,
	    args->out_sync_count);
	if (ret != 0)
		goto out;

	mutex_lock(&agx->lock);
	queue = agx_find_queue(vm, args->queue_id);
	if (queue == NULL || queue->cancel_jobs) {
		ret = -ENOENT;
		goto unlock;
	}
	if (vm->error != 0 || agx->gpu_failed) {
		ret = vm->error != 0 ? vm->error : -EIO;
		goto unlock;
	}
	if (atomic_read(&queue->pending) >= AGX_MAX_PENDING) {
		ret = -EBUSY;
		goto unlock;
	}
	job->done = linux_dma_fence_allocate_software(queue->fence_context,
	    ++queue->fence_seqno);
	if (job->done == NULL) {
		ret = -ENOMEM;
		goto unlock;
	}
	for (i = 0; i < args->out_sync_count; i++) {
		struct agx_output *out = &outputs[i];

		if (out->chain == NULL)
			drm_syncobj_replace_fence(out->syncobj, job->done);
		else {
			drm_syncobj_add_point(out->syncobj, out->chain,
			    job->done, out->point);
			out->chain = NULL;
		}
	}
	job->queue = queue;
	atomic_inc(&vm->pending);
	atomic_inc(&queue->pending);
	queue_work(queue->submit_wq, &job->work);
	job = NULL;
unlock:
	mutex_unlock(&agx->lock);
out:
	if (outputs != NULL) {
		for (i = 0; i < args->out_sync_count; i++) {
			dma_fence_chain_free(outputs[i].chain);
			if (outputs[i].syncobj != NULL)
				drm_syncobj_put(outputs[i].syncobj);
		}
	}
	kfree(outputs);
	kvfree(buf);
	kvfree(syncs);
	if (job != NULL)
		agx_job_free(job);
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
	AGX_IOCTL(GEM_BIND_OBJECT, agx_ioctl_gem_bind_object),
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
	.open = agx_open,
	.postclose = agx_postclose,
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

	if (agx_dev != NULL) {
		drm_dev_unregister(&agx_dev->drm);
	}
}

static int
agxdrm_modevent(module_t module, int event, void *arg)
{
	switch (event) {
	case MOD_LOAD:
		return (-agxdrm_init());
	case MOD_QUIESCE:
	case MOD_UNLOAD:
		if (agx_dev != NULL && (READ_ONCE(agx_dev->gpu_failed) ||
		    atomic_read(&agx_dev->drm.open_count) != 0))
			return (EBUSY);
		if (event == MOD_UNLOAD)
			agxdrm_exit();
		return (0);
	default:
		return (0);
	}
}

static moduledata_t agxdrm_module = {
	.name = "agxdrm",
	.evhand = agxdrm_modevent,
};
DECLARE_MODULE(agxdrm, agxdrm_module, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_DEPEND(agxdrm, drmn, 2, 2, 2);
MODULE_DEPEND(agxdrm, dmabuf, 1, 1, 1);
MODULE_DEPEND(agxdrm, linuxkpi, 1, 1, 1);
MODULE_DEPEND(agxdrm, linuxkpi_video, 1, 1, 1);
