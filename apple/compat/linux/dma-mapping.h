/*
 * DMA API for drm/apple: every device here sits behind a DART, so allocations and mappings go
 * through the device's IOMMU domain (appledrm_iommu.c), not LinuxKPI's busdma path.
 */
#ifndef _APPLEDRM_LINUX_DMA_MAPPING_H_
#define _APPLEDRM_LINUX_DMA_MAPPING_H_
#include_next <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>

#ifndef DMA_ATTR_FORCE_CONTIGUOUS
#define DMA_ATTR_FORCE_CONTIGUOUS	(1UL << 5)
#endif
#ifndef DMA_ATTR_WRITE_COMBINE
#define DMA_ATTR_WRITE_COMBINE		(1UL << 2)
#endif
#ifndef DMA_ATTR_NO_KERNEL_MAPPING
#define DMA_ATTR_NO_KERNEL_MAPPING	(1UL << 4)
#endif
#ifndef DMA_ATTR_SKIP_CPU_SYNC
#define DMA_ATTR_SKIP_CPU_SYNC		(1UL << 5)
#endif
#ifndef DMA_ATTR_PRIVILEGED
#define DMA_ATTR_PRIVILEGED		(1UL << 9)
#endif

void *appledrm_dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp,
    unsigned long attrs);
void appledrm_dma_free_attrs(struct device *dev, size_t size, void *cpu, dma_addr_t handle,
    unsigned long attrs);
int appledrm_dma_get_sgtable_attrs(struct device *dev, struct sg_table *sgt, void *cpu,
    dma_addr_t handle, size_t size, unsigned long attrs);
dma_addr_t appledrm_dma_map_resource(struct device *dev, phys_addr_t phys, size_t size,
    enum dma_data_direction dir, unsigned long attrs);
void appledrm_dma_unmap_resource(struct device *dev, dma_addr_t addr, size_t size,
    enum dma_data_direction dir, unsigned long attrs);
dma_addr_t appledrm_dma_map_single(struct device *dev, void *ptr, size_t size,
    enum dma_data_direction dir);
int appledrm_dma_mapping_error(struct device *dev, dma_addr_t addr);
void appledrm_dma_sync(struct device *dev, dma_addr_t addr, size_t size, bool for_device);
int appledrm_dma_mmap_attrs(struct device *dev, struct vm_area_struct *vma, void *cpu,
    dma_addr_t handle, size_t size, unsigned long attrs);
int appledrm_dma_set_mask(struct device *dev, u64 mask);

#undef dma_alloc_coherent
#undef dma_free_coherent
#undef dma_map_resource
#undef dma_unmap_resource
#undef dma_map_single
#undef dma_mapping_error
#undef dma_set_mask_and_coherent
#undef dma_sync_single_for_device
#undef dma_sync_single_for_cpu
#undef dma_get_sgtable
#define dma_alloc_attrs(d, s, h, g, a)		appledrm_dma_alloc_attrs(d, s, h, g, a)
#define dma_free_attrs(d, s, c, h, a)		appledrm_dma_free_attrs(d, s, c, h, a)
#define dma_alloc_coherent(d, s, h, g)		appledrm_dma_alloc_attrs(d, s, h, g, 0)
#define dma_free_coherent(d, s, c, h)		appledrm_dma_free_attrs(d, s, c, h, 0)
#define dma_alloc_wc(d, s, h, g)		appledrm_dma_alloc_attrs(d, s, h, g, DMA_ATTR_WRITE_COMBINE)
#define dma_free_wc(d, s, c, h)			appledrm_dma_free_attrs(d, s, c, h, DMA_ATTR_WRITE_COMBINE)
#define dma_alloc_noncoherent(d, s, h, dir, g)	appledrm_dma_alloc_attrs(d, s, h, g, 0)
#define dma_free_noncoherent(d, s, c, h, dir)	appledrm_dma_free_attrs(d, s, c, h, 0)
#define dma_get_sgtable_attrs(d, t, c, h, s, a)	appledrm_dma_get_sgtable_attrs(d, t, c, h, s, a)
#define dma_get_sgtable(d, t, c, h, s)		appledrm_dma_get_sgtable_attrs(d, t, c, h, s, 0)
#define dma_map_resource(d, p, s, dir, a)	appledrm_dma_map_resource(d, p, s, dir, a)
#define dma_unmap_resource(d, a, s, dir, at)	appledrm_dma_unmap_resource(d, a, s, dir, at)
#define dma_map_single(d, p, s, dir)		appledrm_dma_map_single(d, p, s, dir)
#define dma_mapping_error(d, a)			appledrm_dma_mapping_error(d, a)
#define dma_set_mask_and_coherent(d, m)		appledrm_dma_set_mask(d, m)
#define dma_sync_single_for_device(d, a, s, dir) appledrm_dma_sync(d, a, s, true)
#define dma_sync_single_for_cpu(d, a, s, dir)	appledrm_dma_sync(d, a, s, false)
#define dma_sync_single_range_for_device(d, a, o, s, dir) appledrm_dma_sync(d, (a) + (o), s, true)
#define dma_sync_single_range_for_cpu(d, a, o, s, dir) appledrm_dma_sync(d, (a) + (o), s, false)
#define dma_mmap_attrs(d, v, c, h, s, a)	appledrm_dma_mmap_attrs(d, v, c, h, s, a)
#define dma_mmap_wc(d, v, c, h, s)		appledrm_dma_mmap_attrs(d, v, c, h, s, DMA_ATTR_WRITE_COMBINE)
#define dma_mmap_coherent(d, v, c, h, s)	appledrm_dma_mmap_attrs(d, v, c, h, s, 0)
#endif
