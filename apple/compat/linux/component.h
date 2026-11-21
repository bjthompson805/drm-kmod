/* Minimal Linux component framework (drivers/base/component.c semantics): appledrm_component.c. */
#ifndef _APPLEDRM_LINUX_COMPONENT_H_
#define _APPLEDRM_LINUX_COMPONENT_H_
#include <linux/device.h>
struct component_ops {
	int (*bind)(struct device *comp, struct device *master, void *master_data);
	void (*unbind)(struct device *comp, struct device *master, void *master_data);
};
struct component_master_ops {
	int (*bind)(struct device *master);
	void (*unbind)(struct device *master);
};
struct component_match;
int component_add(struct device *dev, const struct component_ops *ops);
void component_del(struct device *dev, const struct component_ops *ops);
int component_bind_all(struct device *master, void *master_data);
void component_unbind_all(struct device *master, void *master_data);
int component_compare_of(struct device *dev, void *data);
void component_match_add_release(struct device *master, struct component_match **matchptr,
    void (*release)(struct device *, void *), int (*compare)(struct device *, void *), void *compare_data);
#define component_match_add(master, matchptr, compare, data) \
	component_match_add_release(master, matchptr, NULL, compare, data)
int component_master_add_with_match(struct device *master, const struct component_master_ops *ops,
    struct component_match *match);
void component_master_del(struct device *master, const struct component_master_ops *ops);
#endif
