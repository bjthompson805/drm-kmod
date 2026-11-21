/*
 * Linux OF (device tree) API for drm/apple on FreeBSD, over OFW (dev/ofw).
 * Shadows LinuxKPI's empty <linux/of.h>. A struct device_node wraps one OFW
 * phandle_t; nodes are interned (one struct per node, never freed), so pointer
 * comparisons work and of_node_get/put are no-ops. Implementation: appledrm_of.c.
 */
#ifndef _APPLEDRM_LINUX_OF_H_
#define _APPLEDRM_LINUX_OF_H_

#include <linux/kobject.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/fwnode.h>
#include <linux/cleanup.h>
#include <linux/mod_devicetable.h>

#include <dev/ofw/openfirm.h>

struct device;

struct device_node {
	phandle_t		node;
	const char		*name;		/* node name without unit address */
	const char		*full_name;	/* name@unit */
	struct fwnode_handle	fwnode;
	struct device_node	*parent;	/* interned with the node */
	const char		*type;		/* device_type */
	struct device_node	*hash_next;
};

struct property {
	char		*name;
	int		length;
	void		*value;
	struct property	*next;
};

#define MAX_PHANDLE_ARGS 16
struct of_phandle_args {
	struct device_node	*np;
	int			args_count;
	uint32_t		args[MAX_PHANDLE_ARGS];
};

#ifndef _APPLEDRM_HAVE_OF_DEVICE_ID
struct of_device_id {
	char		name[32];
	char		type[32];
	char		compatible[128];
	const void	*data;
};
#endif

struct device_node *appledrm_of_node(phandle_t node);	/* intern; NULL for 0/-1 */

static inline struct device_node *of_node_get(struct device_node *np) { return np; }
static inline void of_node_put(struct device_node *np) { }
DEFINE_FREE(device_node, struct device_node *, if (_T) of_node_put(_T))

#define dev_of_node(dev) ((dev)->of_node)
#define to_of_node(fw)	((fw) ? container_of((fw), struct device_node, fwnode) : NULL)
#define of_fwnode_handle(np) ((np) ? &(np)->fwnode : NULL)

const char *of_node_full_name(const struct device_node *np);
#define of_node_full_name of_node_full_name
bool of_node_name_eq(const struct device_node *np, const char *name);

struct device_node *of_get_parent(const struct device_node *np);
struct device_node *of_get_next_child(const struct device_node *np, struct device_node *prev);
struct device_node *of_get_next_available_child(const struct device_node *np, struct device_node *prev);
struct device_node *of_get_child_by_name(const struct device_node *np, const char *name);
struct device_node *of_get_compatible_child(const struct device_node *np, const char *compat);
int of_get_available_child_count(const struct device_node *np);
struct device_node *of_find_node_by_path(const char *path);
struct device_node *of_find_compatible_node(struct device_node *from, const char *type,
    const char *compat);
struct device_node *of_find_node_by_phandle(phandle_t handle);

#define for_each_child_of_node(parent, child) \
	for (child = of_get_next_child(parent, NULL); child; child = of_get_next_child(parent, child))
#define for_each_available_child_of_node(parent, child) \
	for (child = of_get_next_available_child(parent, NULL); child; \
	     child = of_get_next_available_child(parent, child))
#define for_each_child_of_node_scoped(parent, child) \
	for (struct device_node *child = of_get_next_child(parent, NULL); child; \
	     child = of_get_next_child(parent, child))
#define for_each_available_child_of_node_scoped(parent, child) \
	for (struct device_node *child = of_get_next_available_child(parent, NULL); child; \
	     child = of_get_next_available_child(parent, child))

bool of_device_is_compatible(const struct device_node *np, const char *compat);
bool of_device_is_available(const struct device_node *np);
bool of_machine_is_compatible(const char *compat);
const struct of_device_id *of_match_node(const struct of_device_id *matches,
    const struct device_node *np);

struct property *of_find_property(const struct device_node *np, const char *name, int *lenp);
const void *of_get_property(const struct device_node *np, const char *name, int *lenp);
static inline bool of_property_present(const struct device_node *np, const char *name)
{ return of_find_property(np, name, NULL) != NULL; }
static inline bool of_property_read_bool(const struct device_node *np, const char *name)
{ return of_property_present(np, name); }

int of_property_read_u32_index(const struct device_node *np, const char *name, u32 index, u32 *out);
int of_property_read_u64_index(const struct device_node *np, const char *name, u32 index, u64 *out);
int of_property_read_variable_u8_array(const struct device_node *np, const char *name, u8 *out,
    size_t sz_min, size_t sz_max);
int of_property_read_variable_u32_array(const struct device_node *np, const char *name, u32 *out,
    size_t sz_min, size_t sz_max);
int of_property_read_variable_u64_array(const struct device_node *np, const char *name, u64 *out,
    size_t sz_min, size_t sz_max);
static inline int of_property_read_u8_array(const struct device_node *np, const char *name,
    u8 *out, size_t sz)
{ int r = of_property_read_variable_u8_array(np, name, out, sz, 0); return r < 0 ? r : 0; }
static inline int of_property_read_u32_array(const struct device_node *np, const char *name,
    u32 *out, size_t sz)
{ int r = of_property_read_variable_u32_array(np, name, out, sz, 0); return r < 0 ? r : 0; }
static inline int of_property_read_u64_array(const struct device_node *np, const char *name,
    u64 *out, size_t sz)
{ int r = of_property_read_variable_u64_array(np, name, out, sz, 0); return r < 0 ? r : 0; }
static inline int of_property_read_u32(const struct device_node *np, const char *name, u32 *out)
{ return of_property_read_u32_array(np, name, out, 1); }
static inline int of_property_read_u64(const struct device_node *np, const char *name, u64 *out)
{ return of_property_read_u64_array(np, name, out, 1); }
static inline int of_property_read_s32(const struct device_node *np, const char *name, s32 *out)
{ return of_property_read_u32(np, name, (u32 *)out); }
int of_property_count_elems_of_size(const struct device_node *np, const char *name, int elem_size);
static inline int of_property_count_u8_elems(const struct device_node *np, const char *name)
{ return of_property_count_elems_of_size(np, name, 1); }
static inline int of_property_count_u32_elems(const struct device_node *np, const char *name)
{ return of_property_count_elems_of_size(np, name, 4); }
static inline int of_property_count_u64_elems(const struct device_node *np, const char *name)
{ return of_property_count_elems_of_size(np, name, 8); }

int of_property_read_string(const struct device_node *np, const char *name, const char **out);
int of_property_read_string_helper(const struct device_node *np, const char *name,
    const char **out, size_t sz, int skip);
static inline int of_property_read_string_index(const struct device_node *np, const char *name,
    int index, const char **out)
{ int r = of_property_read_string_helper(np, name, out, 1, index); return r < 0 ? r : 0; }
static inline int of_property_count_strings(const struct device_node *np, const char *name)
{ return of_property_read_string_helper(np, name, NULL, 0, 0); }
int of_property_match_string(const struct device_node *np, const char *name, const char *string);

const __be32 *of_prop_next_u32(struct property *prop, const __be32 *cur, u32 *pu);
#define of_property_for_each_u32(np, propname, u)				\
	for (struct {struct property *prop; const __be32 *item; } _it =	\
		{of_find_property(np, propname, NULL),				\
		 of_prop_next_u32(_it.prop, NULL, &u)};			\
	     _it.item;								\
	     _it.item = of_prop_next_u32(_it.prop, _it.item, &u))

struct device_node *of_parse_phandle(const struct device_node *np, const char *name, int index);
int of_parse_phandle_with_args(const struct device_node *np, const char *list_name,
    const char *cells_name, int index, struct of_phandle_args *out);
int of_parse_phandle_with_fixed_args(const struct device_node *np, const char *list_name,
    int cell_count, int index, struct of_phandle_args *out);
int of_count_phandle_with_args(const struct device_node *np, const char *list_name,
    const char *cells_name);

int of_n_addr_cells(struct device_node *np);
int of_n_size_cells(struct device_node *np);

extern struct device_node *of_root, *of_chosen;
struct device_node *of_find_matching_node_and_match(struct device_node *from,
    const struct of_device_id *matches, const struct of_device_id **match);
#define for_each_compatible_node(dn, type, compat) \
	for (dn = of_find_compatible_node(NULL, type, compat); dn; \
	     dn = of_find_compatible_node(dn, type, compat))
#define for_each_matching_node(dn, matches) \
	for (dn = of_find_matching_node_and_match(NULL, matches, NULL); dn; \
	     dn = of_find_matching_node_and_match(dn, matches, NULL))
#define for_each_matching_node_and_match(dn, matches, match) \
	for (dn = of_find_matching_node_and_match(NULL, matches, match); dn; \
	     dn = of_find_matching_node_and_match(dn, matches, match))

struct device_node *of_find_node_with_property(struct device_node *from, const char *prop);
struct device_node *of_find_node_by_type(struct device_node *from, const char *type);
#define for_each_node_with_property(dn, prop) \
	for (dn = of_find_node_with_property(NULL, prop); dn; dn = of_find_node_with_property(dn, prop))
#define for_each_node_by_type(dn, type) \
	for (dn = of_find_node_by_type(NULL, type); dn; dn = of_find_node_by_type(dn, type))
#define for_each_compatible_node_scoped(dn, type, compat) \
	for (struct device_node *dn = of_find_compatible_node(NULL, type, compat); dn; \
	     dn = of_find_compatible_node(dn, type, compat))
bool of_node_name_prefix(const struct device_node *np, const char *prefix);
const char *of_prop_next_string(struct property *prop, const char *cur);
#define of_property_for_each_string(np, propname, prop, s)	\
	for (prop = of_find_property(np, propname, NULL),	\
		s = of_prop_next_string(prop, NULL);		\
		s;						\
		s = of_prop_next_string(prop, s))
static inline u64 of_read_number(const __be32 *cell, int size)
{
	u64 r = 0;
	for (; size--; cell++)
		r = (r << 32) | be32_to_cpu(*cell);
	return r;
}

/* of_changeset: property edits applied to the module's property cache (appledrm_of.c). */
struct of_changeset {
	struct list_head	entries;
};
void of_changeset_init(struct of_changeset *ocs);
void of_changeset_destroy(struct of_changeset *ocs);
int of_changeset_apply(struct of_changeset *ocs);
int of_changeset_revert(struct of_changeset *ocs);
int of_changeset_update_prop_string(struct of_changeset *ocs, struct device_node *np,
    const char *prop_name, const char *str);

/* Linux's /aliases lookup. */
struct device_node *of_alias_get_node(const char *alias);

#endif
