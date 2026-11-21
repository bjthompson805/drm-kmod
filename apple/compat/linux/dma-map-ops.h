/* Linux dma-map-ops.h subset used by apple-dart.c's own DCP allocators. */
#ifndef _APPLEDRM_LINUX_DMA_MAP_OPS_H_
#define _APPLEDRM_LINUX_DMA_MAP_OPS_H_
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
bool appledrm_dev_is_dma_coherent(struct device *dev);
#define dev_is_dma_coherent(dev)	appledrm_dev_is_dma_coherent(dev)
pgprot_t appledrm_dma_pgprot(struct device *dev, pgprot_t prot, unsigned long attrs);
#define dma_pgprot(dev, prot, attrs)	appledrm_dma_pgprot(dev, prot, attrs)
void *dma_common_pages_remap(struct page **pages, size_t size, pgprot_t prot, const void *caller);
void dma_common_free_remap(void *cpu_addr, size_t size);
#endif
