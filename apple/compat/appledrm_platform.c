/*
 * Linux platform bus for drm/apple on FreeBSD (see compat/linux/platform_device.h).
 *
 * Binding policy: aurora's DART and mailbox drivers match every DART and ASC mailbox,
 * including the ones FreeBSD's own drivers (NVMe, SMC, USB, N1) already run. So only the
 * display top nodes (appledrm_autoprobe[]) are probed from the tree walk. Every other
 * platform device is created on demand, when a display device references its node
 * (of_find_device_by_node(), "iommus", "mboxes") or creates it (of_platform_device_create()).
 *
 * Each platform device wraps the newbus device FreeBSD made for the same FDT node (left
 * without a driver), or a driverless newbus child of its parent for nodes newbus has none
 * for. Register windows are mapped with pmap_mapdev; IRQs are the newbus device's
 * SYS_RES_IRQ resources, hooked up by the IRQ layer at the end of this file.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/taskqueue.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/bus.h>
#include <machine/resource.h>
#ifndef RF_ACTIVE
#define	RF_ACTIVE	0x0002	/* sys/rman.h; its struct resource clashes with Linux's */
#define	RF_SHAREABLE	0x0004
#endif
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

/* FDT compatibles probed at platform_driver_register() time; the rest bind on demand. */
static const char *const appledrm_autoprobe[] = {
	"apple,display-subsystem",
	"apple,t8142-dcp",
	NULL,
};

static LIST_HEAD(appledrm_drivers);
static LIST_HEAD(appledrm_devices);
static LIST_HEAD(appledrm_deferred);
static DEFINE_MUTEX(appledrm_plat_lock);	/* lists; probes run without it */
static struct sx appledrm_probe_sx;		/* serializes probes (recursive via on-demand) */
SX_SYSINIT_FLAGS(appledrm_probe, &appledrm_probe_sx, "appledrm probe", SX_RECURSE);

struct appledrm_pdev {
	struct platform_device	pdev;
	struct list_head	deferred;	/* on appledrm_deferred while EPROBE_DEFER */
	bool			on_deferred;
	bool			bound;
	struct platform_driver	*drv;
	device_t		own_child;	/* newbus child we added (no FDT newbus device) */
};

#define to_apdev(p)	container_of((p), struct appledrm_pdev, pdev)

int of_dma_configure(struct device *dev, struct device_node *np, bool force_dma);

static void appledrm_irq_setup(struct platform_device *pdev);

bool
appledrm_dev_is_platform(const struct device *dev)
{
	struct appledrm_pdev *ap;
	bool found = false;

	mutex_lock(&appledrm_plat_lock);
	list_for_each_entry(ap, &appledrm_devices, pdev.link)
		if (&ap->pdev.dev == dev) {
			found = true;
			break;
		}
	mutex_unlock(&appledrm_plat_lock);
	return (found);
}

bool
device_is_bound(struct device *dev)
{
	return (appledrm_dev_is_platform(dev) && to_apdev(to_platform_device(dev))->bound);
}

/* ---- newbus lookup ---- */

static device_t
appledrm_find_newbus(device_t bus, phandle_t node)
{
	device_t *kids, found = NULL;
	int n, i;

	if (device_get_children(bus, &kids, &n) != 0)
		return (NULL);
	for (i = 0; i < n && found == NULL; i++) {
		device_t parent = device_get_parent(kids[i]);
		devclass_t pdc = parent != NULL ? device_get_devclass(parent) : NULL;
		const char *pname = pdc != NULL ? devclass_get_name(pdc) : NULL;

		/* only ask OFW-aware buses for a node */
		if (pname != NULL && (strcmp(pname, "simplebus") == 0 ||
		    strcmp(pname, "ofwbus") == 0) && ofw_bus_get_node(kids[i]) == node)
			found = kids[i];
		else
			found = appledrm_find_newbus(kids[i], node);
	}
	free(kids, M_TEMP);
	return (found);
}

/* ---- resources ---- */

static void
appledrm_pdev_resources(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int i, n;

	n = of_property_count_elems_of_size(np, "reg", 4);
	if (n <= 0)
		return;
	n /= of_n_addr_cells(np) + of_n_size_cells(np);
	pdev->resource = kcalloc(n, sizeof(struct resource), GFP_KERNEL);
	for (i = 0; i < n; i++)
		if (of_address_to_resource(np, i, &pdev->resource[pdev->num_resources]) == 0)
			pdev->num_resources++;
}

struct resource *
platform_get_resource(struct platform_device *pdev, unsigned int type, unsigned int num)
{
	u32 i;

	if (type != IORESOURCE_MEM)
		return (NULL);
	for (i = 0; i < pdev->num_resources; i++)
		if (num-- == 0)
			return (&pdev->resource[i]);
	return (NULL);
}

struct resource *
platform_get_resource_byname(struct platform_device *pdev, unsigned int type, const char *name)
{
	u32 i;

	if (type != IORESOURCE_MEM || name == NULL)
		return (NULL);
	for (i = 0; i < pdev->num_resources; i++)
		if (pdev->resource[i].name != NULL && strcmp(pdev->resource[i].name, name) == 0)
			return (&pdev->resource[i]);
	return (NULL);
}

static bool
appledrm_nonposted(struct device_node *np)
{
	for (; np != NULL; np = np->parent)
		if (of_property_present(np, "nonposted-mmio"))
			return (true);
	return (false);
}

void __iomem *
devm_ioremap_resource(struct device *dev, const struct resource *res)
{
	vm_memattr_t ma;

	if (res == NULL)
		return (IOMEM_ERR_PTR(-EINVAL));
	ma = dev != NULL && appledrm_nonposted(dev->of_node) ? VM_MEMATTR_DEVICE_nGnRnE :
	    VM_MEMATTR_DEVICE;
	return ((void __iomem *)pmap_mapdev_attr(res->start, resource_size(res), ma));
}

void __iomem *
devm_platform_get_and_ioremap_resource(struct platform_device *pdev, unsigned int index,
    struct resource **res)
{
	struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, index);

	if (res != NULL)
		*res = r;
	return (devm_ioremap_resource(&pdev->dev, r));
}

void __iomem *
devm_platform_ioremap_resource(struct platform_device *pdev, unsigned int index)
{
	return (devm_platform_get_and_ioremap_resource(pdev, index, NULL));
}

void __iomem *
devm_platform_ioremap_resource_byname(struct platform_device *pdev, const char *name)
{
	return (devm_ioremap_resource(&pdev->dev,
	    platform_get_resource_byname(pdev, IORESOURCE_MEM, name)));
}

void __iomem *
devm_of_iomap(struct device *dev, struct device_node *np, int index, resource_size_t *size)
{
	struct resource r;

	if (of_address_to_resource(np, index, &r) != 0)
		return (IOMEM_ERR_PTR(-EINVAL));
	if (size != NULL)
		*size = resource_size(&r);
	return (devm_ioremap_resource(dev, &r));
}

const void *
of_device_get_match_data(const struct device *dev)
{
	struct platform_device *pdev;

	if (!appledrm_dev_is_platform(dev))
		return (NULL);
	pdev = to_platform_device(__DECONST(struct device *, dev));
	return (pdev->of_match != NULL ? pdev->of_match->data : NULL);
}

/* ---- devices ---- */

static void
appledrm_pdev_release(struct device *dev)
{
	/* Platform devices live until module unload; nothing frees them earlier. */
}

static struct platform_device *
appledrm_pdev_create(struct device_node *np, struct device *parent)
{
	struct appledrm_pdev *ap;
	struct platform_device *pdev;
	device_t bsddev;

	ap = kzalloc(sizeof(*ap), GFP_KERNEL);
	pdev = &ap->pdev;
	pdev->name = np->full_name;
	pdev->id = -1;
	pdev->dev.of_node = np;
	pdev->dev.fwnode = &np->fwnode;
	pdev->dev.parent = parent;
	pdev->dev.release = appledrm_pdev_release;
	INIT_LIST_HEAD(&pdev->dev.irqents);
	spin_lock_init(&pdev->dev.devres_lock);
	INIT_LIST_HEAD(&pdev->dev.devres_head);
	mutex_init(&pdev->dev.lock);
	kobject_init(&pdev->dev.kobj, &linux_dev_ktype);
	kobject_set_name(&pdev->dev.kobj, "%s", np->full_name);

	bus_topo_lock();
	bsddev = appledrm_find_newbus(root_bus, np->node);
	if (bsddev == NULL) {
		device_t pb = parent != NULL && parent->bsddev != NULL ? parent->bsddev : root_bus;

		bsddev = device_add_child(pb, "appledrm_node", DEVICE_UNIT_ANY);
		ap->own_child = bsddev;
	}
	bus_topo_unlock();
	/*
	 * A driverless newbus device has no name, and intr_event_add_handler() refuses a
	 * handler named NULL (device_get_nameunit()): give it a devclass of its own.
	 */
	if (bsddev != NULL && device_get_name(bsddev) == NULL) {
		bus_topo_lock();
		device_set_devclass(bsddev, "appledrm_node");
		bus_topo_unlock();
	}
	if (bsddev != NULL && device_get_desc(bsddev) == NULL)
		device_set_desc_copy(bsddev, np->full_name);
	pdev->dev.bsddev = bsddev;

	appledrm_pdev_resources(pdev);
	appledrm_irq_setup(pdev);

	mutex_lock(&appledrm_plat_lock);
	list_add_tail(&pdev->link, &appledrm_devices);
	mutex_unlock(&appledrm_plat_lock);
	return (pdev);
}

static struct platform_device *
appledrm_pdev_lookup(struct device_node *np)
{
	struct appledrm_pdev *ap;
	struct platform_device *found = NULL;

	mutex_lock(&appledrm_plat_lock);
	list_for_each_entry(ap, &appledrm_devices, pdev.link)
		if (ap->pdev.dev.of_node == np) {
			found = &ap->pdev;
			break;
		}
	mutex_unlock(&appledrm_plat_lock);
	return (found);
}

static struct platform_driver *
appledrm_match_driver(struct device_node *np, const struct of_device_id **match)
{
	struct platform_driver *drv;
	const struct of_device_id *m;

	if (!of_device_is_available(np))
		return (NULL);
	mutex_lock(&appledrm_plat_lock);
	list_for_each_entry(drv, &appledrm_drivers, link) {
		if (drv->driver.of_match_table == NULL)
			continue;
		m = of_match_node(drv->driver.of_match_table, np);
		if (m != NULL) {
			mutex_unlock(&appledrm_plat_lock);
			*match = m;
			return (drv);
		}
	}
	mutex_unlock(&appledrm_plat_lock);
	return (NULL);
}

static void appledrm_retry_deferred(void);

/* Probe @pdev with a matching registered driver. Returns 0, -EPROBE_DEFER, -ENODEV, ... */
static int
appledrm_pdev_probe(struct platform_device *pdev)
{
	struct appledrm_pdev *ap = to_apdev(pdev);
	const struct of_device_id *match = NULL;
	struct platform_driver *drv;
	int ret;

	sx_xlock(&appledrm_probe_sx);
	if (ap->bound) {
		sx_xunlock(&appledrm_probe_sx);
		return (0);
	}
	drv = appledrm_match_driver(pdev->dev.of_node, &match);
	if (drv == NULL || drv->probe == NULL) {
		sx_xunlock(&appledrm_probe_sx);
		return (-ENODEV);
	}
	pdev->of_match = match;
	pdev->dev.driver = &drv->driver;

	/* Linux's platform core sets up the IOMMU before probe (of_dma_configure). */
	if (of_property_present(pdev->dev.of_node, "iommus") && pdev->dev.iommu == NULL) {
		ret = of_dma_configure(&pdev->dev, pdev->dev.of_node, true);
		if (ret != 0)
			goto fail;
	}

	{
		extern int appledrm_errtrace_probing;

		printf("appledrm: %s: %s probe start\n", pdev->dev.of_node->full_name,
		    drv->driver.name);
		atomic_add_int(&appledrm_errtrace_probing, 1);
		ret = drv->probe(pdev);
		atomic_subtract_int(&appledrm_errtrace_probing, 1);
	}
fail:
	if (ret == 0) {
		ap->bound = true;
		ap->drv = drv;
		mutex_lock(&appledrm_plat_lock);
		if (ap->on_deferred) {
			list_del(&ap->deferred);
			ap->on_deferred = false;
		}
		mutex_unlock(&appledrm_plat_lock);
		printf("appledrm: %s bound to %s\n", pdev->dev.of_node->full_name, drv->driver.name);
	} else {
		pdev->dev.driver = NULL;
		lkpi_devres_release_free_list(&pdev->dev);
		INIT_LIST_HEAD(&pdev->dev.devres_head);
		if (ret == -EPROBE_DEFER) {
			printf("appledrm: %s: %s probe deferred\n", pdev->dev.of_node->full_name,
			    drv->driver.name);
			mutex_lock(&appledrm_plat_lock);
			if (!ap->on_deferred) {
				list_add_tail(&ap->deferred, &appledrm_deferred);
				ap->on_deferred = true;
			}
			mutex_unlock(&appledrm_plat_lock);
		} else
			printf("appledrm: %s: %s probe failed: %d\n", pdev->dev.of_node->full_name, drv->driver.name, ret);
	}
	sx_xunlock(&appledrm_probe_sx);
	/* An on-demand provider can finish while its consumer is still probing. */
	if (ret == 0 && !sx_xlocked(&appledrm_probe_sx))
		appledrm_retry_deferred();
	return (ret);
}

/* Linux re-runs deferred probes after any successful probe. */
static void
appledrm_retry_deferred(void)
{
	struct appledrm_pdev *ap;
	bool progress;

	do {
		progress = false;
		mutex_lock(&appledrm_plat_lock);
		list_for_each_entry(ap, &appledrm_deferred, deferred) {
			if (ap->bound)
				continue;
			mutex_unlock(&appledrm_plat_lock);
			if (appledrm_pdev_probe(&ap->pdev) == 0)
				progress = true;
			mutex_lock(&appledrm_plat_lock);
			if (progress)
				break;
		}
		mutex_unlock(&appledrm_plat_lock);
	} while (progress);
}

/* Periodic retry for probes waiting on something that is not a probe (e.g. firmware). */
static struct timeout_task appledrm_defer_task;
static int appledrm_defer_tries;

static void
appledrm_defer_tick(void *arg, int pending)
{
	bool any;

	appledrm_retry_deferred();
	mutex_lock(&appledrm_plat_lock);
	any = !list_empty(&appledrm_deferred);
	mutex_unlock(&appledrm_plat_lock);
	if (any && ++appledrm_defer_tries < 60)
		taskqueue_enqueue_timeout(taskqueue_thread, &appledrm_defer_task, hz);
	else if (any) {
		struct appledrm_pdev *ap;

		mutex_lock(&appledrm_plat_lock);
		list_for_each_entry(ap, &appledrm_deferred, deferred)
			printf("appledrm: %s still deferred after 60 s\n",
			    ap->pdev.dev.of_node->full_name);
		mutex_unlock(&appledrm_plat_lock);
	}
}

static void
appledrm_defer_kick(void)
{
	static bool inited;

	if (!inited) {
		TIMEOUT_TASK_INIT(taskqueue_thread, &appledrm_defer_task, 0, appledrm_defer_tick,
		    NULL);
		inited = true;
	}
	appledrm_defer_tries = 0;
	taskqueue_enqueue_timeout(taskqueue_thread, &appledrm_defer_task, hz);
}

struct platform_device *
of_find_device_by_node(struct device_node *np)
{
	struct platform_device *pdev;
	const struct of_device_id *match;

	if (np == NULL)
		return (NULL);
	pdev = appledrm_pdev_lookup(np);
	if (pdev != NULL)
		return (pdev);
	/* On demand: a provider (DART, mailbox, ...) nobody created yet. */
	if (appledrm_match_driver(np, &match) == NULL)
		return (NULL);
	pdev = appledrm_pdev_create(np, NULL);
	if (appledrm_pdev_probe(pdev) == -EPROBE_DEFER)
		appledrm_defer_kick();
	return (pdev);
}

struct platform_device *
of_platform_device_create(struct device_node *np, const char *bus_id, struct device *parent)
{
	struct platform_device *pdev;

	if (np == NULL || !of_device_is_available(np))
		return (NULL);
	pdev = appledrm_pdev_lookup(np);
	if (pdev == NULL)
		pdev = appledrm_pdev_create(np, parent);
	if (appledrm_pdev_probe(pdev) == -EPROBE_DEFER)
		appledrm_defer_kick();
	return (pdev);
}

void
of_platform_device_destroy_one(struct platform_device *pdev)
{
	struct appledrm_pdev *ap = to_apdev(pdev);

	if (ap->bound && ap->drv != NULL && ap->drv->remove != NULL)
		ap->drv->remove(pdev);
	ap->bound = false;
	lkpi_devres_release_free_list(&pdev->dev);
	INIT_LIST_HEAD(&pdev->dev.devres_head);
}

int
of_platform_device_destroy(struct device *dev, void *data)
{
	if (appledrm_dev_is_platform(dev))
		of_platform_device_destroy_one(to_platform_device(dev));
	return (0);
}

struct device *
driver_find_device(const struct device_driver *drv, struct device *start, const void *data,
    int (*match)(struct device *dev, const void *data))
{
	struct appledrm_pdev *ap;
	struct device *found = NULL;
	bool started = start == NULL;

	mutex_lock(&appledrm_plat_lock);
	list_for_each_entry(ap, &appledrm_devices, pdev.link) {
		if (!started) {
			started = &ap->pdev.dev == start;
			continue;
		}
		if (ap->bound && ap->pdev.dev.driver == drv && match(&ap->pdev.dev, data)) {
			found = &ap->pdev.dev;
			break;
		}
	}
	mutex_unlock(&appledrm_plat_lock);
	if (found != NULL)
		get_device(found);
	return (found);
}

/* ---- drivers ---- */

static void
appledrm_autoprobe_walk(struct platform_driver *drv)
{
	const char *const *c;

	for (c = appledrm_autoprobe; *c != NULL; c++) {
		struct device_node *np = NULL;

		while ((np = of_find_compatible_node(np, NULL, *c)) != NULL) {
			const struct of_device_id *m;
			struct platform_device *pdev;

			if (!of_device_is_available(np))
				continue;
			if (of_match_node(drv->driver.of_match_table, np) == NULL)
				continue;
			m = NULL;
			if (appledrm_match_driver(np, &m) != drv)
				continue;
			pdev = appledrm_pdev_lookup(np);
			if (pdev == NULL)
				pdev = appledrm_pdev_create(np, NULL);
			if (appledrm_pdev_probe(pdev) == -EPROBE_DEFER)
				appledrm_defer_kick();
		}
	}
}

int
platform_driver_register(struct platform_driver *drv)
{
	mutex_lock(&appledrm_plat_lock);
	list_add_tail(&drv->link, &appledrm_drivers);
	mutex_unlock(&appledrm_plat_lock);
	if (drv->driver.of_match_table != NULL)
		appledrm_autoprobe_walk(drv);
	appledrm_retry_deferred();
	return (0);
}

void
platform_driver_unregister(struct platform_driver *drv)
{
	struct appledrm_pdev *ap;

	mutex_lock(&appledrm_plat_lock);
	list_for_each_entry_reverse(ap, &appledrm_devices, pdev.link) {
		if (ap->drv != drv || !ap->bound)
			continue;
		mutex_unlock(&appledrm_plat_lock);
		of_platform_device_destroy_one(&ap->pdev);
		mutex_lock(&appledrm_plat_lock);
	}
	list_del(&drv->link);
	mutex_unlock(&appledrm_plat_lock);
}

int
platform_register_drivers(struct platform_driver * const *drivers, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		platform_driver_register(drivers[i]);
	return (0);
}

void
platform_unregister_drivers(struct platform_driver * const *drivers, unsigned int count)
{
	while (count-- > 0)
		platform_driver_unregister(drivers[count]);
}

/* ---- devres groups: markers only; group release is deferred to device release ---- */

void *
devres_open_group(struct device *dev, void *id, gfp_t gfp)
{
	return (id != NULL ? id : (void *)dev);
}

void
devres_close_group(struct device *dev, void *id)
{
}

void
devres_remove_group(struct device *dev, void *id)
{
}

int
devres_release_group(struct device *dev, void *id)
{
	return (0);
}

void
devm_release_action(struct device *dev, void (*action)(void *), void *data)
{
	/* LinuxKPI cannot remove a single action; run it now (the devres entry stays). */
	action(data);
}

/* ---- IRQs ----
 * Linux IRQ numbers here are APPLEDRM_IRQ_BASE + global index; each maps to one newbus
 * SYS_RES_IRQ resource (FDT "interrupts" index) of a platform device.
 */

#define	APPLEDRM_IRQ_BASE	0x40000
#define	APPLEDRM_IRQ_MAX	256

struct appledrm_irq {
	struct platform_device	*pdev;
	int			rid;
	struct resource		*res;
	void			*tag;
	irq_handler_t		handler;
	irq_handler_t		thread_fn;
	void			*dev_id;
	unsigned long		flags;
	const char		*name;
	bool			requested;
	bool			enabled;
	int			running;	/* handler in progress (synchronize_irq) */
	struct task		disable_task;
};

static struct appledrm_irq appledrm_irqs[APPLEDRM_IRQ_MAX];
static int appledrm_nirqs;
static DEFINE_MUTEX(appledrm_irq_lock);

static void
appledrm_irq_setup(struct platform_device *pdev)
{
	rman_res_t start, count;
	int rid;

	if (pdev->dev.bsddev == NULL)
		return;
	mutex_lock(&appledrm_irq_lock);
	for (rid = 0; appledrm_nirqs < APPLEDRM_IRQ_MAX; rid++) {
		if (bus_get_resource(pdev->dev.bsddev, SYS_RES_IRQ, rid, &start, &count) != 0)
			break;
		if (rid == 0)
			pdev->dev.irq_start = APPLEDRM_IRQ_BASE + appledrm_nirqs;
		appledrm_irqs[appledrm_nirqs].pdev = pdev;
		appledrm_irqs[appledrm_nirqs].rid = rid;
		appledrm_nirqs++;
		pdev->num_irqs++;
	}
	pdev->dev.irq_end = pdev->dev.irq_start + pdev->num_irqs;
	mutex_unlock(&appledrm_irq_lock);
}

static struct appledrm_irq *
appledrm_irq_get(unsigned int irq)
{
	if (irq < APPLEDRM_IRQ_BASE || irq >= APPLEDRM_IRQ_BASE + (unsigned int)appledrm_nirqs)
		return (NULL);
	return (&appledrm_irqs[irq - APPLEDRM_IRQ_BASE]);
}

int
platform_get_irq_optional(struct platform_device *pdev, unsigned int num)
{
	if (num >= pdev->num_irqs)
		return (-ENXIO);
	return (pdev->dev.irq_start + num);
}

int
platform_get_irq(struct platform_device *pdev, unsigned int num)
{
	int irq = platform_get_irq_optional(pdev, num);

	if (irq < 0)
		dev_err(&pdev->dev, "IRQ index %u not found\n", num);
	return (irq);
}

int
platform_get_irq_byname_optional(struct platform_device *pdev, const char *name)
{
	int i = of_property_match_string(pdev->dev.of_node, "interrupt-names", name);

	if (i < 0)
		return (-ENXIO);
	return (platform_get_irq_optional(pdev, i));
}

int
platform_get_irq_byname(struct platform_device *pdev, const char *name)
{
	int irq = platform_get_irq_byname_optional(pdev, name);

	if (irq < 0)
		dev_err(&pdev->dev, "IRQ %s not found\n", name);
	return (irq);
}

int
platform_irq_count(struct platform_device *pdev)
{
	return (pdev->num_irqs);
}

/* Linux semantics in one ithread: hard handler, then the thread if it asked for it. */
static void
appledrm_irq_handler(void *arg)
{
	struct appledrm_irq *ai = arg;
	irqreturn_t r = IRQ_WAKE_THREAD;
	int irq = ai - appledrm_irqs + APPLEDRM_IRQ_BASE;

	atomic_add_int(&ai->running, 1);
	if (ai->handler != NULL)
		r = ai->handler(irq, ai->dev_id);
	if (r == IRQ_WAKE_THREAD && ai->thread_fn != NULL)
		ai->thread_fn(irq, ai->dev_id);
	atomic_subtract_int(&ai->running, 1);
}

static int
appledrm_irq_hook(struct appledrm_irq *ai)
{
	int error;

	if (ai->tag != NULL)
		return (0);
	error = bus_setup_intr(ai->pdev->dev.bsddev, ai->res, INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, appledrm_irq_handler, ai, &ai->tag);
	if (error != 0)
		printf("appledrm: %s: bus_setup_intr(%s) failed: %d\n",
		    ai->pdev->dev.of_node->full_name, ai->name, error);
	if (error == 0) {
		bus_describe_intr(ai->pdev->dev.bsddev, ai->res, ai->tag, "%s", ai->name);
		ai->enabled = true;
	}
	return (-error);
}

static void
appledrm_irq_unhook(struct appledrm_irq *ai)
{
	if (ai->tag != NULL) {
		bus_teardown_intr(ai->pdev->dev.bsddev, ai->res, ai->tag);
		ai->tag = NULL;
	}
	ai->enabled = false;
}

static void
appledrm_irq_disable_task(void *arg, int pending)
{
	struct appledrm_irq *ai = arg;

	mutex_lock(&appledrm_irq_lock);
	if (!ai->enabled)
		appledrm_irq_unhook(ai);
	mutex_unlock(&appledrm_irq_lock);
}

int
appledrm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
    irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev_id)
{
	struct appledrm_irq *ai = appledrm_irq_get(irq);
	int ret = 0;

	if (ai == NULL)
		return (-EINVAL);
	mutex_lock(&appledrm_irq_lock);
	if (ai->requested) {
		mutex_unlock(&appledrm_irq_lock);
		return (-EBUSY);
	}
	ai->res = bus_alloc_resource_any(ai->pdev->dev.bsddev, SYS_RES_IRQ, &ai->rid,
	    RF_ACTIVE | ((flags & IRQF_SHARED) ? RF_SHAREABLE : 0));
	if (ai->res == NULL) {
		mutex_unlock(&appledrm_irq_lock);
		printf("appledrm: %s: IRQ rid %d allocation failed\n",
		    ai->pdev->dev.of_node->full_name, ai->rid);
		return (-ENXIO);
	}
	{
		int appledrm_irq_res_check(device_t, void *, int, const char *);

		appledrm_irq_res_check(ai->pdev->dev.bsddev, ai->res, ai->rid,
		    ai->pdev->dev.of_node->full_name);
	}
	ai->handler = handler;
	ai->thread_fn = thread_fn;
	ai->dev_id = dev_id;
	ai->flags = flags;
	ai->name = name;
	ai->requested = true;
	TASK_INIT(&ai->disable_task, 0, appledrm_irq_disable_task, ai);
	if (!(flags & IRQF_NO_AUTOEN))
		ret = appledrm_irq_hook(ai);
	mutex_unlock(&appledrm_irq_lock);
	return (ret);
}

void
appledrm_free_irq(unsigned int irq, void *dev_id)
{
	struct appledrm_irq *ai = appledrm_irq_get(irq);

	if (ai == NULL || !ai->requested)
		return;
	taskqueue_drain(taskqueue_thread, &ai->disable_task);
	mutex_lock(&appledrm_irq_lock);
	appledrm_irq_unhook(ai);
	bus_release_resource(ai->pdev->dev.bsddev, SYS_RES_IRQ, ai->rid, ai->res);
	ai->res = NULL;
	ai->requested = false;
	mutex_unlock(&appledrm_irq_lock);
}

int
appledrm_enable_irq(unsigned int irq)
{
	struct appledrm_irq *ai = appledrm_irq_get(irq);
	int ret;

	if (ai == NULL || !ai->requested)
		return (-EINVAL);
	mutex_lock(&appledrm_irq_lock);
	ret = appledrm_irq_hook(ai);
	ai->enabled = true;
	mutex_unlock(&appledrm_irq_lock);
	return (ret);
}

void
appledrm_disable_irq(unsigned int irq)
{
	struct appledrm_irq *ai = appledrm_irq_get(irq);

	if (ai == NULL || !ai->requested)
		return;
	mutex_lock(&appledrm_irq_lock);
	appledrm_irq_unhook(ai);	/* bus_teardown_intr waits for a running handler */
	mutex_unlock(&appledrm_irq_lock);
}

void
appledrm_disable_irq_nosync(unsigned int irq)
{
	struct appledrm_irq *ai = appledrm_irq_get(irq);

	if (ai == NULL || !ai->requested)
		return;
	/* May run in the handler itself: tear down from a task. */
	ai->enabled = false;
	taskqueue_enqueue(taskqueue_thread, &ai->disable_task);
}

void
appledrm_synchronize_irq(unsigned int irq)
{
	struct appledrm_irq *ai = appledrm_irq_get(irq);

	while (ai != NULL && atomic_load_int(&ai->running) != 0)
		pause("adrmsyn", 1);
}

static void
appledrm_devm_irq_release(void *arg)
{
	struct appledrm_irq *ai = arg;

	appledrm_free_irq(ai - appledrm_irqs + APPLEDRM_IRQ_BASE, ai->dev_id);
}

int
appledrm_devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
    irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev_id)
{
	int ret = appledrm_request_threaded_irq(dev, irq, handler, thread_fn, flags, name, dev_id);

	if (ret == 0)
		devm_add_action(dev, appledrm_devm_irq_release, appledrm_irq_get(irq));
	return (ret);
}
