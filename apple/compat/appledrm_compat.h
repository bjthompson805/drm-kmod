/* Small Linux helpers LinuxKPI lacks, for drm/apple + soc/apple. Included from the prelude. */
#ifndef _APPLEDRM_COMPAT_H_
#define _APPLEDRM_COMPAT_H_
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/ratelimit.h>

/* arm64 barriers (Linux arch/arm64/include/asm/barrier.h) */
#ifndef dma_rmb
#define dma_rmb()	__asm __volatile("dmb oshld" ::: "memory")
#define dma_wmb()	__asm __volatile("dmb oshst" ::: "memory")
#define dma_mb()	__asm __volatile("dmb osh" ::: "memory")
#endif

#ifndef atomic_dec_return_release
#define atomic_dec_return_release(v)	atomic_dec_return(v)
#endif
#ifndef atomic_inc_return_acquire
#define atomic_inc_return_acquire(v)	atomic_inc_return(v)
#endif

static inline unsigned long int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;
	if (x <= 1)
		return x;
	m = 1UL << (__fls(x) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return y;
}

#ifndef readl_poll_timeout
#define readl_poll_timeout(addr, val, cond, delay_us, timeout_us) \
	read_poll_timeout(readl, val, cond, delay_us, timeout_us, false, addr)
#endif
#ifndef readl_poll_timeout_atomic
#define readl_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) ({		\
	u64 __left = (timeout_us) ? (u64)(timeout_us) * 1000 : 0;			\
	for (;;) {									\
		(val) = readl(addr);							\
		if (cond)								\
			break;								\
		if ((timeout_us) && __left <= (u64)(delay_us) * 1000 + 100) {		\
			(val) = readl(addr);						\
			break;								\
		}									\
		if (delay_us)								\
			udelay(delay_us);						\
		__left -= (u64)(delay_us) * 1000 + 100;					\
	}										\
	(cond) ? 0 : -ETIMEDOUT;							\
})
#endif

#ifndef wait_for_completion_killable_timeout
#define wait_for_completion_killable_timeout(c, t) wait_for_completion_interruptible_timeout(c, t)
#endif

#ifndef dev_info_ratelimited
#define dev_info_ratelimited(dev, fmt, ...) dev_info(dev, fmt, ##__VA_ARGS__)
#endif
#ifndef dev_warn_ratelimited
#define dev_warn_ratelimited(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)
#endif
#ifndef dev_dbg_ratelimited
#define dev_dbg_ratelimited(dev, fmt, ...) do { } while (0)
#endif

#include <vm/vm.h>
#include <vm/pmap.h>
#ifndef __pa
#define __pa(x)	((phys_addr_t)vtophys(x))
#define __va(x)	((void *)PHYS_TO_DMAP((vm_paddr_t)(x)))
#endif

/* runtime PM: devices stay powered (FreeBSD pm is not wired); report active. */
#ifndef pm_runtime_resume_and_get
static inline int pm_runtime_resume_and_get(struct device *dev) { return 0; }
#endif
static inline void pm_runtime_set_suspended(struct device *dev) { }
static inline void pm_runtime_barrier(struct device *dev) { }


/* Linux 6.17+: system_dfl_wq is the unbound default queue. */
#include <linux/workqueue.h>
#ifndef system_dfl_wq
#define system_dfl_wq	system_unbound_wq
#define system_percpu_wq system_wq
#endif

#ifndef DECLARE_COMPLETION_ONSTACK
#define DECLARE_COMPLETION_ONSTACK(work) \
	struct completion work; init_completion(&work)
#endif

#ifndef EUCLEAN
#define EUCLEAN		117
#endif

#ifndef IRQF_NO_SUSPEND
#define IRQF_NO_SUSPEND	0
#endif
#ifndef IRQF_NO_AUTOEN
#define IRQF_NO_AUTOEN	0x00080000	/* appledrm_platform.c: request_irq leaves it disabled */
#endif

/* kunit-only visibility (include/kunit/visibility.h) */
#ifndef VISIBLE_IF_KUNIT
#define VISIBLE_IF_KUNIT static
#define EXPORT_SYMBOL_IF_KUNIT(s)
#endif

#include <linux/suspend.h>
#ifndef pm_resume_via_firmware
static inline bool pm_resume_via_firmware(void) { return false; }
#endif

#ifndef kfifo_reset
#define kfifo_reset(_kf) do { (_kf)->count = (_kf)->first = (_kf)->last = 0; } while (0)
#endif

/* /proc/iomem queries: is [start, start+size) system RAM? (appledrm_platform.c) */
enum { REGION_DISJOINT = 0, REGION_INTERSECTS, REGION_MIXED };
#define IORESOURCE_SYSTEM_RAM	0x01000000
#define IORES_DESC_NONE		0
int region_intersects(resource_size_t start, size_t size, unsigned long flags, unsigned long desc);

/* arm_arch_timer */
static inline u64 arch_timer_read_counter(void)
{
	u64 v;
	__asm __volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}


#include <linux/printk.h>
#include <linux/iosys-map.h>
#include <linux/math64.h>

#ifndef system_freezable_wq
#define system_freezable_wq	system_wq
#endif

/* Linux takes a printf format and args for the name. */
#undef alloc_ordered_workqueue
#define alloc_ordered_workqueue(fmt, flags, ...) \
	linux_create_workqueue_common(kasprintf(GFP_KERNEL, fmt, ##__VA_ARGS__), 1)

#ifndef print_hex_dump_debug
#define print_hex_dump_debug(prefix, type, rs, gs, buf, len, ascii) do { } while (0)
#endif

#ifndef IOSYS_MAP_INIT_VADDR
#define IOSYS_MAP_INIT_VADDR(vaddr_) { .vaddr = (vaddr_), .is_iomem = false }
#endif

#ifndef dma_buf_vmap_unlocked
#define dma_buf_vmap_unlocked(b, m)	dma_buf_vmap(b, m)
#define dma_buf_vunmap_unlocked(b, m)	dma_buf_vunmap(b, m)
#endif

static inline u64 mul_u64_u64_shr(u64 a, u64 b, unsigned int shift)
{
	return (u64)(((unsigned __int128)a * b) >> shift);
}

static inline u32 arch_timer_get_cntfrq(void)
{
	u64 v;
	__asm __volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return (u32)v;
}

#ifndef irqs_disabled
#include <machine/armreg.h>
#define irqs_disabled()	((READ_SPECIALREG(daif) & PSR_I) != 0)
#endif
#ifndef IRQF_ONESHOT
#define IRQF_ONESHOT	0x00002000
#endif

/* Linux 6.10+: disable_work() is cancel + refuse later queueing; FreeBSD: cancel only. */
#ifndef disable_work_sync
#define disable_work_sync(w)	({ cancel_work_sync(w); true; })
#define disable_work(w)		({ cancel_work(w); true; })
#define enable_work(w)		(true)
#define disable_delayed_work_sync(w) ({ cancel_delayed_work_sync(w); true; })
#define enable_delayed_work(w)	(true)
#endif

#define IORESOURCE_MEM_NONPOSTED	0x00000040
/* Apple: nGnRnE device memory for non-posted writes; FreeBSD maps devices nGnRE. */
#define ioremap_np(addr, size)	ioremap(addr, size)

#define DL_FLAG_STATELESS		(1 << 0)
#define DL_FLAG_AUTOREMOVE_CONSUMER	(1 << 1)
#define DL_FLAG_PM_RUNTIME		(1 << 5)
#define DL_FLAG_RPM_ACTIVE		(1 << 6)
#define DL_FLAG_AUTOREMOVE_SUPPLIER	(1 << 4)
struct device_link;
/* Device links only order PM and unbinding; nothing here unbinds or suspends. */
static inline struct device_link *device_link_add(struct device *c, struct device *s, u32 flags)
{ return (struct device_link *)(uintptr_t)1; }
static inline void device_link_del(struct device_link *l) { }

static inline int devm_pm_runtime_enable(struct device *dev) { return 0; }
void devm_release_action(struct device *dev, void (*action)(void *), void *data);
static inline void sysfs_notify(struct kobject *kobj, const char *dir, const char *attr) { }


#ifndef virt_to_phys
#define virt_to_phys(x)	((phys_addr_t)vtophys(x))
#endif
#ifndef cmpxchg64_relaxed
#define cmpxchg64_relaxed(p, o, n)	cmpxchg64(p, o, n)
#endif
#ifndef NOIRQ_SYSTEM_SLEEP_PM_OPS
#define NOIRQ_SYSTEM_SLEEP_PM_OPS(s, r)	.suspend_noirq = (s),
#endif
static inline void vm_flags_mod(struct vm_area_struct *vma, unsigned long set, unsigned long clear)
{
	vma->vm_flags = (vma->vm_flags | set) & ~clear;
}


#include <linux/ktime.h>
#include <linux/fwnode.h>
#include <linux/dma-fence.h>

#ifndef KTIME_MIN
#define KTIME_MIN	(-KTIME_MAX - 1)
#endif
#ifndef PHYS_MASK
#define PHYS_MASK	((1UL << 48) - 1)
#endif
#ifndef pfn_valid
#define pfn_valid(pfn)	(PHYS_TO_VM_PAGE(ptoa(pfn)) != NULL)
#endif

/* Linux of: device tables are only for module autoloading. */
/* LinuxKPI's makes a DRIVER_MODULE per table (named lkpi_<table>, which collide here). */
#undef MODULE_DEVICE_TABLE
#define MODULE_DEVICE_TABLE(_bus, _table)

/* symbol_get(): optional cross-module hooks; none of those modules exist here. */
#define symbol_get(x)	((typeof(&x))NULL)
#define symbol_put(x)	do { } while (0)

/* fwnode: every fwnode here is a struct device_node's (linux/of.h). */
static inline struct fwnode_handle *fwnode_handle_get(struct fwnode_handle *fw) { return fw; }
static inline void fwnode_handle_put(struct fwnode_handle *fw) { }
#define dev_fwnode(dev)	((dev)->of_node ? &(dev)->of_node->fwnode : (dev)->fwnode)

/* device_lock: struct device.lock (m5 LinuxKPI addition) */
#define device_lock(dev)	mutex_lock(&(dev)->lock)
#define device_trylock(dev)	mutex_trylock(&(dev)->lock)
#define device_unlock(dev)	mutex_unlock(&(dev)->lock)
#define device_lock_assert(dev)	do { } while (0)
DEFINE_GUARD(device, struct device *, device_lock(_T), device_unlock(_T))

#ifndef dma_fence_timeline_name
#define dma_fence_timeline_name(f)	((f)->ops->get_timeline_name(f))
#define dma_fence_driver_name(f)	((f)->ops->get_driver_name(f))
#endif

int dma_mmap_pages(struct device *dev, struct vm_area_struct *vma, size_t size, struct page *page);


#ifndef ktime_sub_us
#define ktime_sub_us(kt, us)	ktime_sub_ns(kt, (u64)(us) * NSEC_PER_USEC)
#endif
#ifndef time_after_eq64
#define time_after64(a, b)	((s64)((b) - (a)) < 0)
#define time_before64(a, b)	time_after64(b, a)
#define time_after_eq64(a, b)	((s64)((a) - (b)) >= 0)
#define time_before_eq64(a, b)	time_after_eq64(b, a)
#endif
/* aurora's suspend.h: woke from sleep for nothing user space must see. No sleep here. */
static inline bool pm_dark_wake(void) { return false; }
/* Probe deferral never times out here; keep deferring. */
static inline int driver_deferred_probe_check_state(struct device *dev) { return -EPROBE_DEFER; }

struct drm_crtc;
struct drm_modeset_acquire_ctx;
int drm_atomic_helper_reset_crtc(struct drm_crtc *crtc, struct drm_modeset_acquire_ctx *ctx);


#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/dma-buf.h>
#ifndef dma_buf_begin_cpu_access
#define dma_buf_begin_cpu_access(b, d)	(0)
#define dma_buf_end_cpu_access(b, d)	(0)
#endif
#include "appledrm_alloc_objs.h"
#ifndef IRQF_TRIGGER_RISING
#define IRQF_TRIGGER_NONE	0x00000000
#define IRQF_TRIGGER_RISING	0x00000001
#define IRQF_TRIGGER_FALLING	0x00000002
#define IRQF_TRIGGER_HIGH	0x00000004
#define IRQF_TRIGGER_LOW	0x00000008
#endif
#ifndef disable_delayed_work
#define disable_delayed_work(w)	({ cancel_delayed_work(w); true; })
#endif
static inline void device_set_wakeup_path(struct device *dev) { }
static inline void device_enable_async_suspend(struct device *dev) { }
bool device_is_bound(struct device *dev);

/* devres groups (appledrm_platform.c) */
void *devres_open_group(struct device *dev, void *id, gfp_t gfp);
void devres_close_group(struct device *dev, void *id);
void devres_remove_group(struct device *dev, void *id);
int devres_release_group(struct device *dev, void *id);


#ifndef MODULE_SOFTDEP
#define MODULE_SOFTDEP(x)
#endif
static inline bool pm_suspend_default_s2idle(void) { return true; }
#include <linux/notifier.h>
#ifndef notifier_from_errno
static inline int notifier_from_errno(int err) { return err ? (NOTIFY_STOP_MASK | (NOTIFY_OK - err)) : NOTIFY_OK; }
static inline int notifier_to_errno(int ret) { ret &= ~NOTIFY_STOP_MASK; return ret > NOTIFY_OK ? NOTIFY_OK - ret : 0; }
#endif


#ifndef BITS_TO_U32
#define BITS_TO_U32(nr)	DIV_ROUND_UP(nr, 32)
#define BITS_TO_U64(nr)	DIV_ROUND_UP(nr, 64)
#define BITS_TO_BYTES(nr) DIV_ROUND_UP(nr, 8)
#endif
#ifndef __noreturn
#define __noreturn	__dead2
#endif
/* request_mem_region: exclusivity bookkeeping only; newbus owns the real resources. */
struct resource *appledrm_request_mem_region(resource_size_t start, resource_size_t n, const char *name);
void appledrm_release_mem_region(resource_size_t start, resource_size_t n);
#define request_mem_region(s, n, name)	appledrm_request_mem_region(s, n, name)
#define release_mem_region(s, n)	appledrm_release_mem_region(s, n)


/* ---- mm: struct page is vm_page; RAM without a vm_page (reserved/firmware) is "reserved". ---- */
#include <linux/page.h>
#include <linux/mm.h>
bool appledrm_pa_is_ram(vm_paddr_t pa);	/* inside a hardware RAM region (DMAP-covered) */
#undef pfn_valid
#define pfn_valid(pfn)		appledrm_pa_is_ram(ptoa((vm_paddr_t)(pfn)))
#define pfn_is_map_memory(pfn)	(PHYS_TO_VM_PAGE(ptoa((vm_paddr_t)(pfn))) != NULL)
#ifndef phys_to_virt
#define phys_to_virt(pa)	((void *)PHYS_TO_DMAP((vm_paddr_t)(pa)))
#endif
#define PageReserved(p)		((p) == NULL)
#define page_folio(p)		(p)
#define folio_test_slab(f)	(false)
#define folio_try_get(f)	((f) != NULL)
#ifndef page_count
#define page_count(p)		((p) ? (int)(p)->ref_count : 0)
#endif
#define MEMREMAP_WB		(1 << 0)
#define MEMREMAP_WT		(1 << 1)
#define MEMREMAP_WC		(1 << 2)
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags);
void devm_memunmap(struct device *dev, void *addr);
#define memremap(o, s, f)	((void *)PHYS_TO_DMAP(o))
#define memunmap(a)		do { } while (0)
struct resource *devm_request_mem_region(struct device *dev, resource_size_t start,
    resource_size_t n, const char *name);

#ifndef LIST_HEAD_INIT
#define LIST_HEAD_INIT(name)	LINUX_LIST_HEAD_INIT(name)
#endif

#ifndef for_each_set_bit_from
#define for_each_set_bit_from(bit, addr, size) \
	for ((bit) = find_next_bit((addr), (size), (bit)); (bit) < (size); \
	     (bit) = find_next_bit((addr), (size), (bit) + 1))
#endif

#ifndef FIELD_MAX
#define FIELD_MAX(_mask)	((typeof(_mask))((_mask) >> (__builtin_ffsll(_mask) - 1)))
#endif
#ifndef field_prep
#define field_prep(_mask, _val)	(((typeof(_mask))(_val) << (__builtin_ffsll(_mask) - 1)) & (_mask))
#define field_get(_mask, _reg)	((typeof(_mask))(((_reg) & (_mask)) >> (__builtin_ffsll(_mask) - 1)))
#endif

#ifndef atomic_long_or
#define atomic_long_or(i, v)	atomic_set_long((volatile u_long *)&(v)->counter, (u_long)(i))
#define atomic_long_and(i, v)	atomic_clear_long((volatile u_long *)&(v)->counter, ~(u_long)(i))
#endif

static inline void pm_runtime_irq_safe(struct device *dev) { }

#include <linux/uaccess.h>
static inline int copy_struct_from_user(void *dst, size_t ksize, const void __user *src, size_t usize)
{
	size_t n = min(ksize, usize);
	if (usize < ksize)
		memset((char *)dst + usize, 0, ksize - usize);
	return copy_from_user(dst, src, n) ? -EFAULT : 0;
}
static inline int copy_struct_to_user(void __user *dst, size_t usize, const void *src, size_t ksize,
    bool *ignored_trailing)
{
	size_t n = min(ksize, usize);
	if (ignored_trailing)
		*ignored_trailing = ksize > usize;
	return copy_to_user(dst, src, n) ? -EFAULT : 0;
}


#ifndef DEVICE_ATTR_ADMIN_RO
#define DEVICE_ATTR_ADMIN_RO(_name) struct device_attribute dev_attr_##_name = __ATTR(_name, 0400, _name##_show, NULL)
#define DEVICE_ATTR_ADMIN_RW(_name) struct device_attribute dev_attr_##_name = __ATTR(_name, 0600, _name##_show, _name##_store)
#endif
#ifndef pm_ptr
#define pm_ptr(_ptr)		(_ptr)
#endif
#ifndef pm_sleep_ptr
#define pm_sleep_ptr(_ptr)	(_ptr)
#endif
#ifndef SYSTEM_SLEEP_PM_OPS
#define SYSTEM_SLEEP_PM_OPS(s, r)	.suspend = (s), .resume = (r), .freeze = (s), .thaw = (r), .poweroff = (s), .restore = (r),
#define RUNTIME_PM_OPS(s, r, i)		.runtime_suspend = (s), .runtime_resume = (r), .runtime_idle = (i),
#endif
static inline int pm_runtime_force_suspend(struct device *dev) { return 0; }
static inline int pm_runtime_force_resume(struct device *dev) { return 0; }
static inline bool pm_runtime_status_suspended(struct device *dev) { return false; }
static inline void device_set_awake_path(struct device *dev) { }
struct device_driver;
struct device *driver_find_device(const struct device_driver *drv, struct device *start,
    const void *data, int (*match)(struct device *dev, const void *data));


/* Platform IRQs (appledrm_platform.c); LinuxKPI's IRQ layer only knows PCI devices. */
int appledrm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
    irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev_id);
int appledrm_devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
    irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev_id);
void appledrm_free_irq(unsigned int irq, void *dev_id);
int appledrm_enable_irq(unsigned int irq);
void appledrm_disable_irq(unsigned int irq);
void appledrm_disable_irq_nosync(unsigned int irq);
void appledrm_synchronize_irq(unsigned int irq);
#undef request_irq
#undef request_threaded_irq
#undef devm_request_irq
#undef devm_request_threaded_irq
#undef free_irq
#undef enable_irq
#undef disable_irq
#undef disable_irq_nosync
#undef synchronize_irq
#define request_irq(i, h, f, n, d)		appledrm_request_threaded_irq(NULL, i, h, NULL, f, n, d)
#define request_threaded_irq(i, h, t, f, n, d)	appledrm_request_threaded_irq(NULL, i, h, t, f, n, d)
#define devm_request_irq(dv, i, h, f, n, d)	appledrm_devm_request_threaded_irq(dv, i, h, NULL, f, n, d)
#define devm_request_threaded_irq(dv, i, h, t, f, n, d) \
	appledrm_devm_request_threaded_irq(dv, i, h, t, f, n, d)
/* Object-like, so struct members of the same name are renamed consistently too. */
#define free_irq				appledrm_free_irq
#define enable_irq				appledrm_enable_irq
#define disable_irq				appledrm_disable_irq
#define disable_irq_nosync			appledrm_disable_irq_nosync
#define synchronize_irq				appledrm_synchronize_irq


/* debugfs lives in lindebugfs.ko, which the MFS does not load: no-op files. */
#include <linux/debugfs.h>
#define debugfs_create_file(n, m, p, d, f)	((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_remove_recursive(d)		do { } while (0)


/* module_param_cb(): drm/apple's tunables keep their defaults (no sysctl export). */
#include <linux/moduleparam.h>
struct kernel_param;
struct kernel_param_ops {
	unsigned int flags;
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
	void (*free)(void *arg);
};
struct kernel_param {
	const char *name;
	const struct kernel_param_ops *ops;
	void *arg;
};
int param_set_int(const char *val, const struct kernel_param *kp);
int param_get_int(char *buffer, const struct kernel_param *kp);
#ifndef module_param_cb
#define module_param_cb(name, ops, arg, perm)	\
	static const struct kernel_param __param_##name __unused = { #name, ops, arg }
#endif

#ifndef strstarts
static inline bool strstarts(const char *str, const char *prefix)
{
	return strncmp(str, prefix, strlen(prefix)) == 0;
}
#endif

#endif
