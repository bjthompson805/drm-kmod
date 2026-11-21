#ifndef _APPLEDRM_LINUX_OF_PLATFORM_H_
#define _APPLEDRM_LINUX_OF_PLATFORM_H_
#include <linux/of.h>
#include <linux/platform_device.h>
struct platform_device *of_platform_device_create(struct device_node *np, const char *bus_id,
    struct device *parent);
void of_platform_device_destroy_one(struct platform_device *pdev);
int of_platform_device_destroy(struct device *dev, void *data);
struct platform_device *of_find_device_by_node(struct device_node *np);
#endif
