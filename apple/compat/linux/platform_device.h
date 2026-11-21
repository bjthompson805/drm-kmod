/*
 * Linux platform bus for drm/apple on FreeBSD. Shadows LinuxKPI's stub header.
 * A newbus driver (appledrm_platform.c) attaches to FDT nodes whose compatible
 * matches a registered platform_driver's of_match_table and calls its probe with
 * a platform_device wrapping the newbus device. Child platform devices made by
 * of_platform_device_create() get a newbus child without a driver.
 */
#ifndef _APPLEDRM_LINUX_PLATFORM_DEVICE_H_
#define _APPLEDRM_LINUX_PLATFORM_DEVICE_H_

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/mod_devicetable.h>
#include <linux/pci.h>


struct platform_device {
	const char		*name;
	int			id;
	bool			id_auto;
	struct device		dev;
	u32			num_resources;
	struct resource		*resource;	/* IORESOURCE_MEM by "reg" index, names from reg-names */
	u32			num_irqs;
	const struct of_device_id *of_match;
	struct list_head	link;		/* appledrm_platform.c list of all devices */
	bool			created;	/* by of_platform_device_create() */
};

struct platform_driver {
	int			(*probe)(struct platform_device *);
	void			(*remove)(struct platform_device *);
	void			(*shutdown)(struct platform_device *);
	bool			driver_managed_dma;
	struct device_driver	driver;
	struct list_head	link;
};

#define to_platform_device(x)	container_of((x), struct platform_device, dev)
bool appledrm_dev_is_platform(const struct device *dev);
#define dev_is_platform(dev)	appledrm_dev_is_platform(dev)

static inline void *platform_get_drvdata(const struct platform_device *pdev)
{ return dev_get_drvdata(&pdev->dev); }
static inline void platform_set_drvdata(struct platform_device *pdev, void *data)
{ dev_set_drvdata(&pdev->dev, data); }
static inline void *dev_get_platdata(const struct device *dev) { return NULL; }

int platform_driver_register(struct platform_driver *drv);
void platform_driver_unregister(struct platform_driver *drv);
#define __platform_driver_register(drv, owner) platform_driver_register(drv)
int platform_register_drivers(struct platform_driver * const *drivers, unsigned int count);
void platform_unregister_drivers(struct platform_driver * const *drivers, unsigned int count);

struct resource *platform_get_resource(struct platform_device *pdev, unsigned int type, unsigned int num);
struct resource *platform_get_resource_byname(struct platform_device *pdev, unsigned int type,
    const char *name);
int platform_get_irq(struct platform_device *pdev, unsigned int num);
int platform_get_irq_optional(struct platform_device *pdev, unsigned int num);
int platform_get_irq_byname(struct platform_device *pdev, const char *name);
int platform_get_irq_byname_optional(struct platform_device *pdev, const char *name);
int platform_irq_count(struct platform_device *pdev);

void __iomem *devm_ioremap_resource(struct device *dev, const struct resource *res);
void __iomem *devm_platform_ioremap_resource(struct platform_device *pdev, unsigned int index);
void __iomem *devm_platform_ioremap_resource_byname(struct platform_device *pdev, const char *name);
void __iomem *devm_platform_get_and_ioremap_resource(struct platform_device *pdev,
    unsigned int index, struct resource **res);
void __iomem *devm_of_iomap(struct device *dev, struct device_node *np, int index, resource_size_t *size);

const void *of_device_get_match_data(const struct device *dev);
#define device_get_match_data(dev) of_device_get_match_data(dev)

#define module_platform_driver(__drv)					\
	static int __init __drv##_init(void) { return platform_driver_register(&(__drv)); } \
	module_init(__drv##_init);					\
	static void __exit __drv##_exit(void) { platform_driver_unregister(&(__drv)); } \
	module_exit(__drv##_exit)

#define builtin_platform_driver(__drv) module_platform_driver(__drv)

#endif
