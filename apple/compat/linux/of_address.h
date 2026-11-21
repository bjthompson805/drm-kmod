#ifndef _APPLEDRM_LINUX_OF_ADDRESS_H_
#define _APPLEDRM_LINUX_OF_ADDRESS_H_
#include <linux/of.h>
#include <linux/ioport.h>
/* "reg" entry @index of @np, translated through the parents' "ranges" to a CPU address. */
int of_address_to_resource(struct device_node *np, int index, struct resource *r);
void __iomem *of_iomap(struct device_node *np, int index);
u64 of_translate_address(struct device_node *np, const __be32 *addr);
#endif
