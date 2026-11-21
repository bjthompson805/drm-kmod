/*
 * The part of Linux's IOMMU core (drivers/iommu/iommu.c, of_iommu.c, dma-iommu.c) that
 * aurora's apple-dart.c and the drm/apple DMA calls need, on FreeBSD.
 *
 * Probe order follows aurora's iommu.c: of_xlate per "iommus" entry, probe_device,
 * device_group, default domain (def_domain_type / domain_alloc_paging), firmware
 * mappings from get_resv_regions (direct and translated, iommu_create_device_fw_mappings),
 * then attach_dev. DMA domains get an IOVA arena (vmem) over the domain aperture with the
 * reserved regions carved out, like iommu_dma_init_domain().
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/iommu.h>
#include <linux/io-pgtable.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/scatterlist.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include "iommu-pages.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vmem.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <machine/cpufunc.h>

struct iommu_group {
	struct kobject		kobj;
	int			refs;
	struct list_head	devices;	/* struct group_dev */
	struct iommu_domain	*domain;
	struct iommu_domain	*default_domain;
	void			*iommu_data;
	void			(*iommu_data_release)(void *iommu_data);
	struct mutex		mutex;
};

struct group_dev {
	struct list_head	list;
	struct device		*dev;
};

/* struct dev_iommu plus what Linux keeps in struct device (iommu_group). */
struct appledrm_dev_iommu {
	struct dev_iommu	di;
	struct iommu_group	*group;
};

/* DMA cookie: IOVA arena of a DMA domain. */
struct appledrm_dma_cookie {
	vmem_t			*arena;
	dma_addr_t		base, end;
	unsigned long		granule;
};

static LIST_HEAD(iommu_device_list);
static DEFINE_MUTEX(iommu_lock);

/* ---- iommu_device ---- */

int
iommu_device_register(struct iommu_device *iommu, const struct iommu_ops *ops, struct device *hwdev)
{
	iommu->ops = ops;
	iommu->dev = hwdev;
	if (iommu->fwnode == NULL && hwdev != NULL)
		iommu->fwnode = dev_fwnode(hwdev);
	iommu->ready = true;
	mutex_lock(&iommu_lock);
	list_add_tail(&iommu->list, &iommu_device_list);
	mutex_unlock(&iommu_lock);
	return (0);
}

void
iommu_device_unregister(struct iommu_device *iommu)
{
	mutex_lock(&iommu_lock);
	list_del(&iommu->list);
	mutex_unlock(&iommu_lock);
}

int
iommu_device_sysfs_add(struct iommu_device *iommu, struct device *parent,
    const struct attribute_group **groups, const char *fmt, ...)
{
	return (0);
}

void
iommu_device_sysfs_remove(struct iommu_device *iommu)
{
}

static struct iommu_device *
iommu_from_fwnode(struct fwnode_handle *fw)
{
	struct iommu_device *iommu, *found = NULL;

	mutex_lock(&iommu_lock);
	list_for_each_entry(iommu, &iommu_device_list, list)
		if (iommu->fwnode == fw) {
			found = iommu;
			break;
		}
	mutex_unlock(&iommu_lock);
	return (found);
}

/* ---- dev_iommu / fwspec ---- */

static struct dev_iommu *
dev_iommu_get(struct device *dev)
{
	struct appledrm_dev_iommu *adi;

	if (dev->iommu != NULL)
		return (dev->iommu);
	adi = kzalloc(sizeof(*adi), GFP_KERNEL);
	mutex_init(&adi->di.lock);
	dev->iommu = &adi->di;
	return (dev->iommu);
}

#define	dev_group(dev)	(container_of((dev)->iommu, struct appledrm_dev_iommu, di)->group)

void
dev_iommu_priv_set(struct device *dev, void *priv)
{
	dev_iommu_get(dev)->priv = priv;
}

int
iommu_fwspec_init(struct device *dev, struct fwnode_handle *iommu_fwnode)
{
	struct iommu_fwspec *fwspec;

	dev_iommu_get(dev);
	fwspec = dev->iommu->fwspec;
	if (fwspec != NULL)
		return (fwspec->iommu_fwnode == iommu_fwnode ? 0 : -EINVAL);
	fwspec = kzalloc(sizeof(*fwspec), GFP_KERNEL);
	fwspec->iommu_fwnode = iommu_fwnode;
	dev->iommu->fwspec = fwspec;
	return (0);
}

int
iommu_fwspec_add_ids(struct device *dev, const u32 *ids, int num_ids)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev), *n;
	int i;

	if (fwspec == NULL)
		return (-EINVAL);
	n = kzalloc(sizeof(*n) + sizeof(u32) * (fwspec->num_ids + num_ids), GFP_KERNEL);
	memcpy(n, fwspec, sizeof(*n) + sizeof(u32) * fwspec->num_ids);
	for (i = 0; i < num_ids; i++)
		n->ids[n->num_ids + i] = ids[i];
	n->num_ids += num_ids;
	dev->iommu->fwspec = n;
	kfree(fwspec);
	return (0);
}

/* ---- groups ---- */

struct iommu_group *
iommu_group_alloc(void)
{
	struct iommu_group *g = kzalloc(sizeof(*g), GFP_KERNEL);

	g->refs = 1;
	INIT_LIST_HEAD(&g->devices);
	mutex_init(&g->mutex);
	return (g);
}

struct iommu_group *
generic_device_group(struct device *dev)
{
	return (iommu_group_alloc());
}

struct iommu_group *
pci_device_group(struct device *dev)
{
	return (generic_device_group(dev));
}

struct iommu_group *
iommu_group_ref_get(struct iommu_group *group)
{
	if (group != NULL)
		group->refs++;
	return (group);
}

struct iommu_group *
iommu_group_get(struct device *dev)
{
	if (dev->iommu == NULL)
		return (NULL);
	return (iommu_group_ref_get(dev_group(dev)));
}

void
iommu_group_put(struct iommu_group *group)
{
	if (group != NULL)
		group->refs--;
}

void *
iommu_group_get_iommudata(struct iommu_group *group)
{
	return (group->iommu_data);
}

void
iommu_group_set_iommudata(struct iommu_group *group, void *iommu_data,
    void (*release)(void *iommu_data))
{
	group->iommu_data = iommu_data;
	group->iommu_data_release = release;
}

int
iommu_group_for_each_dev(struct iommu_group *group, void *data,
    int (*fn)(struct device *, void *))
{
	struct group_dev *gd;
	int ret = 0;

	mutex_lock(&group->mutex);
	list_for_each_entry(gd, &group->devices, list) {
		ret = fn(gd->dev, data);
		if (ret != 0)
			break;
	}
	mutex_unlock(&group->mutex);
	return (ret);
}

struct iommu_domain *
iommu_get_domain_for_dev(struct device *dev)
{
	struct iommu_group *g;

	if (dev == NULL || dev->iommu == NULL || (g = dev_group(dev)) == NULL)
		return (NULL);
	return (g->domain);
}

int
iommu_device_use_default_domain(struct device *dev)
{
	return (0);
}

void
iommu_device_unuse_default_domain(struct device *dev)
{
}

/* ---- reserved regions ---- */

struct iommu_resv_region *
iommu_alloc_resv_region_tr(phys_addr_t start, dma_addr_t dva_start, size_t length, int prot,
    enum iommu_resv_type type, gfp_t gfp)
{
	struct iommu_resv_region *region = kzalloc(sizeof(*region), gfp);

	if (region == NULL)
		return (NULL);
	INIT_LIST_HEAD(&region->list);
	region->start = start;
	if (type == IOMMU_RESV_TRANSLATED)
		region->dva = dva_start;
	region->length = length;
	region->prot = prot;
	region->type = type;
	return (region);
}

struct iommu_resv_region *
iommu_alloc_resv_region(phys_addr_t start, size_t length, int prot, enum iommu_resv_type type,
    gfp_t gfp)
{
	if (type == IOMMU_RESV_TRANSLATED)
		return (NULL);
	return (iommu_alloc_resv_region_tr(start, 0, length, prot, type, gfp));
}

void
iommu_get_resv_regions(struct device *dev, struct list_head *list)
{
	const struct iommu_ops *ops;

	if (dev->iommu == NULL || dev->iommu->iommu_dev == NULL)
		return;
	ops = dev->iommu->iommu_dev->ops;
	if (ops->get_resv_regions != NULL)
		ops->get_resv_regions(dev, list);
}

void
iommu_put_resv_regions(struct device *dev, struct list_head *list)
{
	struct iommu_resv_region *entry, *next;

	list_for_each_entry_safe(entry, next, list, list) {
		if (entry->free != NULL)
			entry->free(dev, entry);
		else
			kfree(entry);
	}
}

/* dma-iommu: MSI regions; Apple DARTs have none here. */
void
iommu_dma_get_resv_regions(struct device *dev, struct list_head *list)
{
}

/* ---- map / unmap (iommu.c) ---- */

static size_t
iommu_pgsize(struct iommu_domain *domain, unsigned long iova, phys_addr_t paddr, size_t size,
    size_t *count)
{
	unsigned int pgsize_idx, pgsize_idx_next;
	unsigned long pgsizes;
	size_t offset, pgsize, pgsize_next;
	unsigned long addr_merge = paddr | iova;

	pgsizes = domain->pgsize_bitmap & GENMASK(__fls(size), 0);
	if (likely(addr_merge))
		pgsizes &= GENMASK(__ffs(addr_merge), 0);
	BUG_ON(!pgsizes);
	pgsize_idx = __fls(pgsizes);
	pgsize = BIT(pgsize_idx);
	if (!count)
		return (pgsize);
	pgsizes = domain->pgsize_bitmap & ~GENMASK(pgsize_idx, 0);
	if (!pgsizes)
		goto out_set_count;
	pgsize_idx_next = __ffs(pgsizes);
	pgsize_next = BIT(pgsize_idx_next);
	if ((iova ^ paddr) & (pgsize_next - 1))
		goto out_set_count;
	offset = pgsize_next - (addr_merge & (pgsize_next - 1));
	if (offset + pgsize_next <= size)
		size = offset;
out_set_count:
	*count = size >> pgsize_idx;
	return (pgsize);
}

int
iommu_map(struct iommu_domain *domain, unsigned long iova, phys_addr_t paddr, size_t size,
    int prot, gfp_t gfp)
{
	const struct iommu_domain_ops *ops = domain->ops;
	unsigned long orig_iova = iova;
	unsigned int min_pagesz;
	size_t orig_size = size;
	int ret = 0;

	if (ops->map_pages == NULL || domain->pgsize_bitmap == 0)
		return (-ENODEV);
	min_pagesz = 1 << __ffs(domain->pgsize_bitmap);
	if (!IS_ALIGNED(iova | paddr | size, min_pagesz)) {
		pr_err("appledrm iommu: unaligned map: iova 0x%lx pa %#jx size 0x%zx min_pagesz 0x%x\n",
		    iova, (uintmax_t)paddr, size, min_pagesz);
		return (-EINVAL);
	}
	while (size) {
		size_t pgsize, count, mapped = 0;

		pgsize = iommu_pgsize(domain, iova, paddr, size, &count);
		ret = ops->map_pages(domain, iova, paddr, pgsize, count, prot, gfp, &mapped);
		size -= mapped;
		if (ret)
			break;
		iova += mapped;
		paddr += mapped;
	}
	if (ret) {
		iommu_unmap(domain, orig_iova, orig_size - size);
		return (ret);
	}
	if (ops->iotlb_sync_map != NULL)
		ret = ops->iotlb_sync_map(domain, orig_iova, orig_size);
	return (ret);
}

size_t
iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	const struct iommu_domain_ops *ops = domain->ops;
	struct iommu_iotlb_gather gather;
	size_t unmapped_page, unmapped = 0;
	unsigned int min_pagesz;

	if (ops->unmap_pages == NULL || domain->pgsize_bitmap == 0)
		return (0);
	min_pagesz = 1 << __ffs(domain->pgsize_bitmap);
	if (!IS_ALIGNED(iova | size, min_pagesz))
		return (0);
	iommu_iotlb_gather_init(&gather);
	while (unmapped < size) {
		size_t pgsize, count;

		pgsize = iommu_pgsize(domain, iova, iova, size - unmapped, &count);
		unmapped_page = ops->unmap_pages(domain, iova, pgsize, count, &gather);
		if (!unmapped_page)
			break;
		iova += unmapped_page;
		unmapped += unmapped_page;
	}
	if (ops->iotlb_sync != NULL)
		ops->iotlb_sync(domain, &gather);
	return (unmapped);
}

ssize_t
iommu_map_sg(struct iommu_domain *domain, unsigned long iova, struct scatterlist *sg,
    unsigned int nents, int prot, gfp_t gfp)
{
	size_t len = 0, mapped = 0;
	phys_addr_t start = 0;
	unsigned int i = 0;
	int ret;

	/* Linux's loop: the extra pass at i == nents flushes the last run. */
	while (i <= nents) {
		phys_addr_t s_phys = sg_phys(sg);

		if (len && s_phys != start + len) {
			ret = iommu_map(domain, iova + mapped, start, len, prot, gfp);
			if (ret)
				goto out_err;
			mapped += len;
			len = 0;
		}
		if (len) {
			len += sg->length;
		} else {
			len = sg->length;
			start = s_phys;
		}
		if (++i < nents)
			sg = sg_next(sg);
	}
	return (mapped);
out_err:
	iommu_unmap(domain, iova, mapped);
	return (ret);
}

phys_addr_t
iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	if (domain == NULL)
		return (0);
	if (domain->type == IOMMU_DOMAIN_IDENTITY)
		return (iova);
	if (domain->type == IOMMU_DOMAIN_BLOCKED || domain->ops->iova_to_phys == NULL)
		return (0);
	return (domain->ops->iova_to_phys(domain, iova));
}

/* ---- page-table pages (iommu-pages.h) ---- */

struct pt_alloc {
	struct list_head	list;
	void			*va;
	size_t			size;
};
static LIST_HEAD(pt_allocs);
static DEFINE_SPINLOCK(pt_lock);

void *
iommu_alloc_pages_node_sz(int nid, gfp_t gfp, size_t size)
{
	struct pt_alloc *pa;
	void *va;

	size = roundup_pow_of_two(MAX(size, PAGE_SIZE));
	va = kmem_alloc_contig(size, M_ZERO | ((gfp & M_NOWAIT) ? M_NOWAIT : M_WAITOK), 0,
	    ~(vm_paddr_t)0, size, 0, VM_MEMATTR_DEFAULT);
	if (va == NULL)
		return (NULL);
	pa = kzalloc(sizeof(*pa), gfp);
	pa->va = va;
	pa->size = size;
	spin_lock(&pt_lock);
	list_add(&pa->list, &pt_allocs);
	spin_unlock(&pt_lock);
	return (va);
}

void
iommu_free_pages(void *virt)
{
	struct pt_alloc *pa, *found = NULL;

	if (virt == NULL)
		return;
	spin_lock(&pt_lock);
	list_for_each_entry(pa, &pt_allocs, list)
		if (pa->va == virt) {
			found = pa;
			list_del(&pa->list);
			break;
		}
	spin_unlock(&pt_lock);
	if (found == NULL) {
		pr_err("appledrm iommu: freeing unknown table page %p\n", virt);
		return;
	}
	kmem_free(found->va, found->size);
	kfree(found);
}

/* ---- io-pgtable (io-pgtable.c) ---- */

struct io_pgtable_ops *
alloc_io_pgtable_ops(enum io_pgtable_fmt fmt, struct io_pgtable_cfg *cfg, void *cookie)
{
	struct io_pgtable *iop;

	if (fmt != APPLE_DART && fmt != APPLE_DART2)
		return (NULL);
	iop = io_pgtable_apple_dart_init_fns.alloc(cfg, cookie);
	if (iop == NULL)
		return (NULL);
	iop->fmt = fmt;
	iop->cookie = cookie;
	iop->cfg = *cfg;
	return (&iop->ops);
}

void
free_io_pgtable_ops(struct io_pgtable_ops *ops)
{
	struct io_pgtable *iop;

	if (ops == NULL)
		return;
	iop = io_pgtable_ops_to_pgtable(ops);
	io_pgtable_tlb_flush_all(iop);
	io_pgtable_apple_dart_init_fns.free(iop);
}

/* ---- device probe (iommu.c / of_iommu.c) ---- */


static int
iommu_create_device_fw_mappings(struct iommu_domain *domain, struct device *dev)
{
	struct iommu_resv_region *entry;
	LIST_HEAD(mappings);
	unsigned long pg_size;
	int ret = 0;

	pg_size = domain->pgsize_bitmap ? 1UL << __ffs(domain->pgsize_bitmap) : 0;
	if (WARN_ON_ONCE(iommu_is_dma_domain(domain) && !pg_size))
		return (-EINVAL);

	iommu_get_resv_regions(dev, &mappings);
	list_for_each_entry(entry, &mappings, list) {
		dma_addr_t start, end, addr, iova;
		size_t map_size = 0;

		if (entry->type == IOMMU_RESV_DIRECT)
			dev->iommu->require_direct = 1;
		if (entry->type == IOMMU_RESV_TRANSLATED)
			dev->iommu->require_translated = 1;
		if ((entry->type != IOMMU_RESV_DIRECT &&
		    entry->type != IOMMU_RESV_DIRECT_RELAXABLE &&
		    entry->type != IOMMU_RESV_TRANSLATED) || !iommu_is_dma_domain(domain))
			continue;

		start = ALIGN(entry->start, pg_size);
		end = ALIGN(entry->start + entry->length, pg_size);
		iova = entry->type == IOMMU_RESV_TRANSLATED ? ALIGN(entry->dva, pg_size) : start;

		for (addr = start; addr <= end; addr += pg_size, iova += pg_size) {
			phys_addr_t phys_addr;

			if (addr == end)
				goto map_end;
			phys_addr = iommu_iova_to_phys(domain, iova ? iova : 1);
			if (!phys_addr) {
				map_size += pg_size;
				continue;
			}
map_end:
			if (map_size) {
				ret = iommu_map(domain, iova - map_size, addr - map_size, map_size,
				    entry->prot, GFP_KERNEL);
				if (ret)
					goto out;
				map_size = 0;
			}
		}
	}
out:
	iommu_put_resv_regions(dev, &mappings);
	return (ret);
}

/* iommu_dma_init_domain(): IOVA arena over the aperture minus reserved regions. */
static int
iommu_dma_init_domain(struct iommu_domain *domain, struct device *dev)
{
	struct appledrm_dma_cookie *c;
	struct iommu_resv_region *entry;
	LIST_HEAD(resv);
	dma_addr_t base, end;

	if (domain->iova_cookie != NULL)
		return (0);
	base = domain->geometry.aperture_start;
	end = domain->geometry.aperture_end;
	if (end <= base)
		return (-EINVAL);
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	c->granule = 1UL << __ffs(domain->pgsize_bitmap);
	/* Linux never hands out IOVA 0; start one granule up. */
	if (base == 0)
		base = c->granule;
	c->base = base;
	c->end = end;
	c->arena = vmem_create("appledrm iova", base, end - base + 1, c->granule, 0, M_WAITOK);
	iommu_get_resv_regions(dev, &resv);
	list_for_each_entry(entry, &resv, list) {
		dma_addr_t s = entry->type == IOMMU_RESV_TRANSLATED ? entry->dva : entry->start;
		dma_addr_t e = s + entry->length;
		vmem_addr_t got;

		if (entry->type == IOMMU_RESV_MSI || entry->type == IOMMU_RESV_SW_MSI)
			continue;
		s = rounddown(s, c->granule);
		e = roundup(e, c->granule);
		if (e <= base || s > end)
			continue;
		s = MAX(s, base);
		e = MIN(e, end + 1);
		/* Overlapping regions may already be carved out; ignore failures. */
		vmem_xalloc(c->arena, e - s, 0, 0, 0, s, e, M_NOWAIT | M_BESTFIT, &got);
	}
	iommu_put_resv_regions(dev, &resv);
	domain->cookie_type = IOMMU_COOKIE_DMA_IOVA;
	domain->iova_cookie = (void *)c;
	return (0);
}

static int
appledrm_iommu_probe_device(struct device *dev)
{
	const struct iommu_ops *ops = dev->iommu->iommu_dev->ops;
	struct iommu_group *group;
	struct iommu_domain *domain;
	struct iommu_device *iommu;
	struct group_dev *gd;
	unsigned int type;
	int ret;

	iommu = ops->probe_device(dev);
	if (IS_ERR(iommu))
		return (PTR_ERR(iommu));
	dev->iommu->iommu_dev = iommu;

	group = ops->device_group(dev);
	if (IS_ERR_OR_NULL(group)) {
		ret = group == NULL ? -ENODEV : PTR_ERR(group);
		goto err_release;
	}
	dev_group(dev) = group;
	gd = kzalloc(sizeof(*gd), GFP_KERNEL);
	gd->dev = dev;
	mutex_lock(&group->mutex);
	list_add_tail(&gd->list, &group->devices);

	domain = group->default_domain;
	if (domain == NULL) {
		type = ops->def_domain_type != NULL ? ops->def_domain_type(dev) : 0;
		printf("appledrm: %s: def_domain_type %#x (PAGE_SIZE %d)\n",
		    dev->of_node != NULL ? dev->of_node->full_name : "?", type, (int)PAGE_SIZE);
		if (type == 0)
			type = IOMMU_DOMAIN_DMA;
		if (type == IOMMU_DOMAIN_IDENTITY && ops->identity_domain != NULL)
			domain = ops->identity_domain;
		else if (type == IOMMU_DOMAIN_BLOCKED && ops->blocked_domain != NULL)
			domain = ops->blocked_domain;
		else {
			if (ops->domain_alloc_paging == NULL) {
				mutex_unlock(&group->mutex);
				ret = -ENODEV;
				goto err_release;
			}
			domain = ops->domain_alloc_paging(dev);
			if (IS_ERR_OR_NULL(domain)) {
				mutex_unlock(&group->mutex);
				ret = domain == NULL ? -ENOMEM : PTR_ERR(domain);
				goto err_release;
			}
			domain->type = type;
			domain->owner = ops;
			if (domain->ops == NULL)
				domain->ops = ops->default_domain_ops;
		}
		group->default_domain = domain;
	}

	ret = iommu_create_device_fw_mappings(domain, dev);
	if (ret == 0 && group->domain != domain) {
		ret = domain->ops->attach_dev(domain, dev, group->domain);
		if (ret == 0)
			group->domain = domain;
	} else if (ret == 0)
		ret = domain->ops->attach_dev(domain, dev, NULL);
	mutex_unlock(&group->mutex);
	if (ret != 0) {
		dev_err(dev, "IOMMU attach failed: %d\n", ret);
		goto err_release;
	}
	if (iommu_is_dma_domain(domain))
		iommu_dma_init_domain(domain, dev);
	if (ops->probe_finalize != NULL)
		ops->probe_finalize(dev);
	return (0);

err_release:
	if (ops->release_device != NULL)
		ops->release_device(dev);
	dev->iommu->iommu_dev = NULL;
	return (ret);
}

/* Undo of_dma_configure() state after a failure, so the next probe redoes it. */
static void
appledrm_iommu_forget(struct device *dev)
{
	struct appledrm_dev_iommu *adi;

	if (dev->iommu == NULL)
		return;
	adi = container_of(dev->iommu, struct appledrm_dev_iommu, di);
	kfree(adi->di.fwspec);
	dev->iommu = NULL;
	kfree(adi);
}

/* of_iommu_configure(): xlate every "iommus" entry, then probe. */
int
of_dma_configure(struct device *dev, struct device_node *np, bool force_dma)
{
	struct of_phandle_args args;
	struct iommu_device *iommu = NULL;
	int idx, ret;

	if (dev->iommu != NULL && dev->iommu->iommu_dev != NULL)
		return (0);
	for (idx = 0; of_parse_phandle_with_args(np, "iommus", "#iommu-cells", idx, &args) == 0;
	    idx++) {
		struct platform_device *ipdev;

		/* The DART binds on demand; probing may defer. */
		ipdev = of_find_device_by_node(args.np);
		if (ipdev == NULL || !device_is_bound(&ipdev->dev) ||
		    (iommu = iommu_from_fwnode(&args.np->fwnode)) == NULL) {
			appledrm_iommu_forget(dev);
			return (-EPROBE_DEFER);
		}
		ret = iommu_fwspec_init(dev, &args.np->fwnode);
		if (ret == 0)
			ret = iommu->ops->of_xlate(dev, &args);
		if (ret != 0) {
			appledrm_iommu_forget(dev);
			return (ret);
		}
	}
	if (idx == 0)
		return (0);		/* no IOMMU: DMA is IOVA == PA */
	dev->iommu->iommu_dev = iommu;
	ret = appledrm_iommu_probe_device(dev);
	if (ret != 0)
		appledrm_iommu_forget(dev);
	return (ret);
}


/* LinuxKPI's device_iommu_mapped() is a stub returning false (appledrm_compat.h redirects). */
bool
appledrm_device_iommu_mapped(struct device *dev)
{
	return (dev->iommu != NULL && dev->iommu->iommu_dev != NULL && dev_group(dev) != NULL);
}

/* ---- DMA API over the device's DMA domain (dma-iommu.c in spirit) ---- */

struct dma_alloc {
	struct list_head	list;
	struct device		*dev;
	void			*cpu;
	vm_paddr_t		pa;
	dma_addr_t		iova;
	size_t			size;		/* rounded to the IOVA granule */
	bool			mmio;		/* dma_map_resource / map_single: no memory owned */
};
static LIST_HEAD(dma_allocs);
static DEFINE_SPINLOCK(dma_lock);

bool
appledrm_dev_is_dma_coherent(struct device *dev)
{
	struct device_node *np;

	for (np = dev->of_node; np != NULL; np = np->parent)
		if (of_property_present(np, "dma-coherent"))
			return (true);
	return (false);
}

pgprot_t
appledrm_dma_pgprot(struct device *dev, pgprot_t prot, unsigned long attrs)
{
	if (appledrm_dev_is_dma_coherent(dev))
		return (prot);
	return (pgprot_writecombine(prot));
}

void *
dma_common_pages_remap(struct page **pages, size_t size, pgprot_t prot, const void *caller)
{
	return (vmap(pages, PAGE_ALIGN(size) >> PAGE_SHIFT, VM_MAP, prot));
}

void
dma_common_free_remap(void *cpu_addr, size_t size)
{
	vunmap(cpu_addr);
}

static struct appledrm_dma_cookie *
dev_dma_cookie(struct device *dev, struct iommu_domain **domp)
{
	struct iommu_domain *d = iommu_get_domain_for_dev(dev);

	*domp = d;
	if (d == NULL || !iommu_is_dma_domain(d))
		return (NULL);
	return ((struct appledrm_dma_cookie *)d->iova_cookie);
}

static int
dma_prot(struct device *dev, bool mmio)
{
	int prot = IOMMU_READ | IOMMU_WRITE;

	if (mmio)
		prot |= IOMMU_MMIO;
	else if (appledrm_dev_is_dma_coherent(dev))
		prot |= IOMMU_CACHE;
	return (prot);
}

/* Map [pa, pa+size) for @dev; returns the IOVA (or IOVA==PA without a DMA domain). */
static dma_addr_t
dma_map_phys(struct device *dev, vm_paddr_t pa, size_t *sizep, bool mmio)
{
	struct iommu_domain *d;
	struct appledrm_dma_cookie *c = dev_dma_cookie(dev, &d);
	vmem_addr_t iova;
	size_t size;

	if (c == NULL)
		return (d == NULL ? (dma_addr_t)pa : DMA_MAPPING_ERROR);
	size = roundup(*sizep, c->granule);
	if (vmem_alloc(c->arena, size, M_NOWAIT | M_BESTFIT, &iova) != 0) {
		dev_err(dev, "out of IOVA space (0x%zx)\n", size);
		return (DMA_MAPPING_ERROR);
	}
	if (iommu_map(d, iova, pa, size, dma_prot(dev, mmio), GFP_KERNEL) != 0) {
		vmem_free(c->arena, iova, size);
		return (DMA_MAPPING_ERROR);
	}
	*sizep = size;
	return (iova);
}

static void
dma_unmap_iova(struct device *dev, dma_addr_t iova, size_t size)
{
	struct iommu_domain *d;
	struct appledrm_dma_cookie *c = dev_dma_cookie(dev, &d);

	if (c == NULL)
		return;
	iommu_unmap(d, iova, size);
	vmem_free(c->arena, iova, size);
}

static void
dma_track(struct device *dev, void *cpu, vm_paddr_t pa, dma_addr_t iova, size_t size, bool mmio)
{
	struct dma_alloc *a = kzalloc(sizeof(*a), GFP_KERNEL);

	a->dev = dev;
	a->cpu = cpu;
	a->pa = pa;
	a->iova = iova;
	a->size = size;
	a->mmio = mmio;
	spin_lock(&dma_lock);
	list_add(&a->list, &dma_allocs);
	spin_unlock(&dma_lock);
}

static struct dma_alloc *
dma_find(struct device *dev, dma_addr_t iova, bool remove)
{
	struct dma_alloc *a, *found = NULL;

	spin_lock(&dma_lock);
	list_for_each_entry(a, &dma_allocs, list)
		if (a->dev == dev && iova >= a->iova && iova < a->iova + a->size) {
			found = a;
			if (remove)
				list_del(&a->list);
			break;
		}
	spin_unlock(&dma_lock);
	return (found);
}

void *
appledrm_dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp,
    unsigned long attrs)
{
	vm_memattr_t ma;
	size_t asize;
	void *cpu;
	vm_paddr_t pa;
	dma_addr_t iova;

	asize = roundup(size, PAGE_SIZE);
	{
		struct iommu_domain *d;
		struct appledrm_dma_cookie *c = dev_dma_cookie(dev, &d);

		if (c != NULL)
			asize = roundup(size, c->granule);
	}
	ma = appledrm_dev_is_dma_coherent(dev) ? VM_MEMATTR_DEFAULT : VM_MEMATTR_WRITE_COMBINING;
	cpu = kmem_alloc_contig(asize, M_ZERO | ((gfp & M_NOWAIT) ? M_NOWAIT : M_WAITOK), 0,
	    ~(vm_paddr_t)0, PAGE_SIZE, 0, ma);
	if (cpu == NULL)
		return (NULL);
	pa = vtophys(cpu);
	iova = dma_map_phys(dev, pa, &asize, false);
	if (iova == DMA_MAPPING_ERROR) {
		kmem_free(cpu, asize);
		return (NULL);
	}
	dma_track(dev, cpu, pa, iova, asize, false);
	*handle = iova;
	return (cpu);
}

void
appledrm_dma_free_attrs(struct device *dev, size_t size, void *cpu, dma_addr_t handle,
    unsigned long attrs)
{
	struct dma_alloc *a = dma_find(dev, handle, true);

	if (a == NULL) {
		dev_err(dev, "dma_free of unknown IOVA %#jx\n", (uintmax_t)handle);
		return;
	}
	dma_unmap_iova(dev, a->iova, a->size);
	if (!a->mmio)
		kmem_free(a->cpu, a->size);
	kfree(a);
}

int
appledrm_dma_get_sgtable_attrs(struct device *dev, struct sg_table *sgt, void *cpu,
    dma_addr_t handle, size_t size, unsigned long attrs)
{
	struct dma_alloc *a = dma_find(dev, handle, false);
	int ret;

	if (a == NULL)
		return (-ENXIO);
	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret)
		return (ret);
	sg_set_page(sgt->sgl, PHYS_TO_VM_PAGE(a->pa + (handle - a->iova)), PAGE_ALIGN(size), 0);
	return (0);
}

dma_addr_t
appledrm_dma_map_resource(struct device *dev, phys_addr_t phys, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{
	vm_paddr_t pa = trunc_page(phys);
	size_t msize = round_page(phys + size) - pa;
	dma_addr_t iova = dma_map_phys(dev, pa, &msize, true);

	if (iova == DMA_MAPPING_ERROR)
		return (iova);
	dma_track(dev, NULL, pa, iova, msize, true);
	return (iova + (phys - pa));
}

void
appledrm_dma_unmap_resource(struct device *dev, dma_addr_t addr, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{
	struct dma_alloc *a = dma_find(dev, addr, true);

	if (a == NULL)
		return;
	dma_unmap_iova(dev, a->iova, a->size);
	kfree(a);
}

dma_addr_t
appledrm_dma_map_single(struct device *dev, void *ptr, size_t size, enum dma_data_direction dir)
{
	vm_paddr_t phys = vtophys(ptr), pa = trunc_page(phys);
	size_t msize = round_page(phys + size) - pa;
	dma_addr_t iova;

	if (!appledrm_dev_is_dma_coherent(dev))
		cpu_dcache_wbinv_range(ptr, size);
	iova = dma_map_phys(dev, pa, &msize, false);
	if (iova == DMA_MAPPING_ERROR)
		return (iova);
	dma_track(dev, NULL, pa, iova, msize, true);
	return (iova + (phys - pa));
}

int
appledrm_dma_mapping_error(struct device *dev, dma_addr_t addr)
{
	return (addr == DMA_MAPPING_ERROR ? -ENOMEM : 0);
}

void
appledrm_dma_sync(struct device *dev, dma_addr_t addr, size_t size, bool for_device)
{
	struct dma_alloc *a;
	void *va;

	if (appledrm_dev_is_dma_coherent(dev))
		return;
	a = dma_find(dev, addr, false);
	if (a == NULL)
		return;
	va = (void *)PHYS_TO_DMAP(a->pa + (addr - a->iova));
	if (for_device)
		cpu_dcache_wb_range(va, size);
	else
		cpu_dcache_inv_range(va, size);
}

int
appledrm_dma_mmap_attrs(struct device *dev, struct vm_area_struct *vma, void *cpu,
    dma_addr_t handle, size_t size, unsigned long attrs)
{
	struct dma_alloc *a = dma_find(dev, handle, false);

	if (a == NULL)
		return (-ENXIO);
	if (!appledrm_dev_is_dma_coherent(dev) || (attrs & DMA_ATTR_WRITE_COMBINE))
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return (remap_pfn_range(vma, vma->vm_start, atop(a->pa + (handle - a->iova)),
	    vma->vm_end - vma->vm_start, vma->vm_page_prot));
}

int
dma_mmap_pages(struct device *dev, struct vm_area_struct *vma, size_t size, struct page *page)
{
	return (remap_pfn_range(vma, vma->vm_start, atop(VM_PAGE_TO_PHYS(page)),
	    vma->vm_end - vma->vm_start, vma->vm_page_prot));
}

int
appledrm_dma_set_mask(struct device *dev, u64 mask)
{
	return (0);
}
