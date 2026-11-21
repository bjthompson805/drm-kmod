#ifndef _APPLEDRM_LINUX_OF_DEVICE_H_
#define _APPLEDRM_LINUX_OF_DEVICE_H_
#include <linux/of.h>
#include <linux/platform_device.h>
/* Set up @dev's DMA/IOMMU from @np's "iommus" (appledrm_iommu.c). */
int of_dma_configure(struct device *dev, struct device_node *np, bool force_dma);
#endif
