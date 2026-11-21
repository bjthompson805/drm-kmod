#ifndef _APPLEDRM_LINUX_OF_GRAPH_H_
#define _APPLEDRM_LINUX_OF_GRAPH_H_
#include <linux/of.h>
struct device_node *of_graph_get_endpoint_by_regs(const struct device_node *parent, int port_reg, int reg);
struct device_node *of_graph_get_next_endpoint(const struct device_node *parent, struct device_node *prev);
struct device_node *of_graph_get_next_port(const struct device_node *parent, struct device_node *prev);
struct device_node *of_graph_get_next_port_endpoint(const struct device_node *port, struct device_node *prev);
struct device_node *of_graph_get_port_by_id(struct device_node *node, u32 id);
struct device_node *of_graph_get_remote_port_parent(const struct device_node *node);
struct device_node *of_graph_get_remote_endpoint(const struct device_node *node);
#define for_each_endpoint_of_node(parent, child) \
	for (child = of_graph_get_next_endpoint(parent, NULL); child != NULL; \
	     child = of_graph_get_next_endpoint(parent, child))
#define for_each_of_graph_port(parent, child) \
	for (struct device_node *child __free(device_node) = of_graph_get_next_port(parent, NULL); \
	     child != NULL; child = of_graph_get_next_port(parent, child))
#define for_each_of_graph_port_endpoint(parent, child) \
	for (struct device_node *child __free(device_node) = of_graph_get_next_port_endpoint(parent, NULL); \
	     child != NULL; child = of_graph_get_next_port_endpoint(parent, child))
#endif
