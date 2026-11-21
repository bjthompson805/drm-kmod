#ifndef _APPLEDRM_LINUX_MFD_SYSCON_H_
#define _APPLEDRM_LINUX_MFD_SYSCON_H_
#include <linux/regmap.h>
struct device_node;
struct regmap *syscon_node_to_regmap(struct device_node *np);
struct regmap *syscon_regmap_lookup_by_compatible(const char *s);
struct regmap *syscon_regmap_lookup_by_phandle_args(struct device_node *np, const char *property,
    int arg_count, unsigned int *out_args);
#endif
