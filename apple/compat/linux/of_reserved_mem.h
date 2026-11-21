/* /reserved-memory lookup (appledrm_of.c). */
#ifndef _APPLEDRM_LINUX_OF_RESERVED_MEM_H_
#define _APPLEDRM_LINUX_OF_RESERVED_MEM_H_
#include <linux/of.h>
struct reserved_mem {
	const char		*name;
	unsigned long		fdt_node;
	const struct reserved_mem_ops *ops;
	phys_addr_t		base;
	phys_addr_t		size;
	void			*priv;
};
struct reserved_mem *of_reserved_mem_lookup(struct device_node *np);
#endif
