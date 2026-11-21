#ifndef _APPLEDRM_DRM_OF_H_
#define _APPLEDRM_DRM_OF_H_
#include_next <drm/drm_of.h>
#include <linux/component.h>
#include <linux/of.h>
void drm_of_component_match_add(struct device *master, struct component_match **matchptr,
    int (*compare)(struct device *, void *), struct device_node *node);
#endif
