/*
 * Linux OF API over FreeBSD OFW for drm/apple (see compat/linux/of.h).
 *
 * Nodes are interned: one struct device_node per OFW node, created on first use and
 * kept for the module's lifetime, so Linux code can compare node pointers. Properties
 * are read once into struct property records hung off the node (Linux code keeps
 * pointers into property values).
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>

struct appledrm_node {
	struct device_node	np;
	struct property		*props;
	struct reserved_mem	*rmem;
};

#define	NODE_HASH	256
static struct device_node *node_hash[NODE_HASH];
static struct mtx node_mtx;
MTX_SYSINIT(appledrm_of, &node_mtx, "appledrm OF nodes", MTX_DEF);

struct device_node *of_root, *of_chosen;

static char *
appledrm_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = kmalloc(n, GFP_KERNEL);

	memcpy(d, s, n);
	return (d);
}

struct device_node *
appledrm_of_node(phandle_t node)
{
	struct appledrm_node *an;
	struct device_node *np;
	char buf[128], *at;
	phandle_t parent;
	u_int h;

	if (node == 0 || node == (phandle_t)-1)
		return (NULL);
	h = (u_int)node % NODE_HASH;
	mtx_lock(&node_mtx);
	for (np = node_hash[h]; np != NULL; np = np->hash_next)
		if (np->node == node)
			break;
	mtx_unlock(&node_mtx);
	if (np != NULL)
		return (np);

	an = kzalloc(sizeof(*an), GFP_KERNEL);
	np = &an->np;
	np->node = node;
	if (OF_getprop(node, "name", buf, sizeof(buf)) <= 0)
		strlcpy(buf, node == OF_peer(0) ? "" : "unknown", sizeof(buf));
	np->full_name = appledrm_strdup(buf);
	at = strchr(buf, '@');
	if (at != NULL)
		*at = '\0';
	np->name = appledrm_strdup(buf);
	if (OF_getprop(node, "device_type", buf, sizeof(buf)) > 0)
		np->type = appledrm_strdup(buf);
	parent = OF_parent(node);
	np->parent = parent != 0 ? appledrm_of_node(parent) : NULL;

	mtx_lock(&node_mtx);
	/* Lost a race: keep the first. */
	{
		struct device_node *o;

		for (o = node_hash[h]; o != NULL; o = o->hash_next)
			if (o->node == node) {
				mtx_unlock(&node_mtx);
				return (o);
			}
	}
	np->hash_next = node_hash[h];
	node_hash[h] = np;
	mtx_unlock(&node_mtx);
	return (np);
}

static void
appledrm_of_init(void)
{
	if (of_root == NULL)
		of_root = appledrm_of_node(OF_peer(0));
	if (of_chosen == NULL)
		of_chosen = appledrm_of_node(OF_finddevice("/chosen"));
}
SYSINIT(appledrm_of_init, SI_SUB_DRIVERS, SI_ORDER_FIRST, (void (*)(void *))appledrm_of_init, NULL);

const char *
of_node_full_name(const struct device_node *np)
{
	return (np != NULL ? np->full_name : "<no-node>");
}

bool
of_node_name_eq(const struct device_node *np, const char *name)
{
	return (np != NULL && name != NULL && strcmp(np->name, name) == 0);
}

bool
of_node_name_prefix(const struct device_node *np, const char *prefix)
{
	return (np != NULL && prefix != NULL &&
	    strncmp(np->name, prefix, strlen(prefix)) == 0);
}

struct device_node *
of_get_parent(const struct device_node *np)
{
	return (np != NULL ? np->parent : NULL);
}

struct device_node *
of_get_next_child(const struct device_node *np, struct device_node *prev)
{
	if (np == NULL)
		return (NULL);
	return (appledrm_of_node(prev == NULL ? OF_child(np->node) : OF_peer(prev->node)));
}

bool
of_device_is_available(const struct device_node *np)
{
	const char *status;
	int len;

	if (np == NULL)
		return (false);
	/* Through the property cache, so of_changeset edits count. */
	status = of_get_property(np, "status", &len);
	if (status == NULL || len <= 0)
		return (true);
	return (strcmp(status, "okay") == 0 || strcmp(status, "ok") == 0);
}

struct device_node *
of_get_next_available_child(const struct device_node *np, struct device_node *prev)
{
	struct device_node *c = prev;

	do {
		c = of_get_next_child(np, c);
	} while (c != NULL && !of_device_is_available(c));
	return (c);
}

struct device_node *
of_get_child_by_name(const struct device_node *np, const char *name)
{
	struct device_node *c;

	for_each_child_of_node(np, c)
		if (of_node_name_eq(c, name))
			return (c);
	return (NULL);
}

bool
of_device_is_compatible(const struct device_node *np, const char *compat)
{
	return (np != NULL && compat != NULL &&
	    ofw_bus_node_is_compatible(np->node, compat) != 0);
}

struct device_node *
of_get_compatible_child(const struct device_node *np, const char *compat)
{
	struct device_node *c;

	for_each_child_of_node(np, c)
		if (of_device_is_compatible(c, compat))
			return (c);
	return (NULL);
}

int
of_get_available_child_count(const struct device_node *np)
{
	struct device_node *c;
	int n = 0;

	for_each_available_child_of_node(np, c)
		n++;
	return (n);
}

bool
of_machine_is_compatible(const char *compat)
{
	appledrm_of_init();
	return (of_device_is_compatible(of_root, compat));
}

const struct of_device_id *
of_match_node(const struct of_device_id *m, const struct device_node *np)
{
	if (m == NULL || np == NULL)
		return (NULL);
	/* Linux scores by position in the node's compatible list; take the first entry. */
	{
		char *compat, *p;
		ssize_t len;

		len = OF_getprop_alloc(np->node, "compatible", (void **)&compat);
		if (len > 0) {
			for (p = compat; p < compat + len; p += strlen(p) + 1) {
				const struct of_device_id *e;

				for (e = m; e->compatible[0] || e->name[0] || e->type[0]; e++)
					if (e->compatible[0] && strcmp(e->compatible, p) == 0) {
						OF_prop_free(compat);
						return (e);
					}
			}
			OF_prop_free(compat);
		}
	}
	for (; m->compatible[0] || m->name[0] || m->type[0]; m++) {
		if (m->compatible[0])
			continue;
		if (m->name[0] && strcmp(m->name, np->name) != 0)
			continue;
		if (m->type[0] && (np->type == NULL || strcmp(m->type, np->type) != 0))
			continue;
		return (m);
	}
	return (NULL);
}

/* Depth-first successor of @n in the whole tree. */
static phandle_t
of_next_node(phandle_t n)
{
	phandle_t c;

	if ((c = OF_child(n)) != 0)
		return (c);
	while (n != 0) {
		if ((c = OF_peer(n)) != 0)
			return (c);
		n = OF_parent(n);
	}
	return (0);
}

static struct device_node *
of_find_next(struct device_node *from, bool (*pred)(phandle_t, const void *), const void *arg)
{
	phandle_t n;

	appledrm_of_init();
	n = from != NULL ? of_next_node(from->node) : OF_peer(0);
	for (; n != 0; n = of_next_node(n))
		if (pred(n, arg))
			return (appledrm_of_node(n));
	return (NULL);
}

struct compat_type {
	const char *type, *compat;
};

static bool
pred_compat(phandle_t n, const void *arg)
{
	const struct compat_type *ct = arg;
	char buf[64];

	if (ct->type != NULL && (OF_getprop(n, "device_type", buf, sizeof(buf)) <= 0 ||
	    strcmp(buf, ct->type) != 0))
		return (false);
	return (ofw_bus_node_is_compatible(n, ct->compat) != 0);
}

struct device_node *
of_find_compatible_node(struct device_node *from, const char *type, const char *compat)
{
	struct compat_type ct = { type, compat };

	return (of_find_next(from, pred_compat, &ct));
}

static bool
pred_prop(phandle_t n, const void *arg)
{
	return (OF_hasprop(n, arg) != 0);
}

struct device_node *
of_find_node_with_property(struct device_node *from, const char *prop)
{
	return (of_find_next(from, pred_prop, prop));
}

static bool
pred_type(phandle_t n, const void *arg)
{
	char buf[64];

	return (OF_getprop(n, "device_type", buf, sizeof(buf)) > 0 && strcmp(buf, arg) == 0);
}

struct device_node *
of_find_node_by_type(struct device_node *from, const char *type)
{
	return (of_find_next(from, pred_type, type));
}

struct device_node *
of_find_matching_node_and_match(struct device_node *from, const struct of_device_id *matches,
    const struct of_device_id **match)
{
	struct device_node *np = from;
	const struct of_device_id *m;

	for (;;) {
		phandle_t n;

		appledrm_of_init();
		n = np != NULL ? of_next_node(np->node) : OF_peer(0);
		if (n == 0)
			return (NULL);
		np = appledrm_of_node(n);
		if ((m = of_match_node(matches, np)) != NULL) {
			if (match != NULL)
				*match = m;
			return (np);
		}
	}
}

struct device_node *
of_find_node_by_path(const char *path)
{
	phandle_t n = OF_finddevice(path);

	return (n == -1 ? NULL : appledrm_of_node(n));
}

struct device_node *
of_find_node_by_phandle(phandle_t handle)
{
	phandle_t n = OF_node_from_xref(handle);

	return (n == 0 || n == -1 ? NULL : appledrm_of_node(n));
}

struct device_node *
of_alias_get_node(const char *alias)
{
	char path[256];
	phandle_t a = OF_finddevice("/aliases");

	if (a == -1 || OF_getprop(a, alias, path, sizeof(path)) <= 0)
		return (NULL);
	return (of_find_node_by_path(path));
}

/* ---- properties ---- */

struct property *
of_find_property(const struct device_node *cnp, const char *name, int *lenp)
{
	struct appledrm_node *an;
	struct device_node *np = __DECONST(struct device_node *, cnp);
	struct property *pp;
	ssize_t len;
	void *val;

	if (np == NULL || name == NULL)
		return (NULL);
	an = container_of(np, struct appledrm_node, np);
	mtx_lock(&node_mtx);
	for (pp = an->props; pp != NULL; pp = pp->next)
		if (strcmp(pp->name, name) == 0)
			break;
	mtx_unlock(&node_mtx);
	if (pp == NULL) {
		len = OF_getproplen(np->node, name);
		if (len < 0)
			return (NULL);
		pp = kzalloc(sizeof(*pp), GFP_KERNEL);
		pp->name = appledrm_strdup(name);
		pp->length = len;
		val = kzalloc(len + 1, GFP_KERNEL);
		if (len > 0)
			OF_getprop(np->node, name, val, len);
		pp->value = val;
		mtx_lock(&node_mtx);
		pp->next = an->props;
		an->props = pp;
		mtx_unlock(&node_mtx);
	}
	if (pp->length < 0)
		return (NULL);
	if (lenp != NULL)
		*lenp = pp->length;
	return (pp);
}

const void *
of_get_property(const struct device_node *np, const char *name, int *lenp)
{
	struct property *pp = of_find_property(np, name, lenp);

	return (pp != NULL ? pp->value : NULL);
}

static const void *
of_find_property_value_of_size(const struct device_node *np, const char *name, u32 min, u32 max,
    size_t *len)
{
	struct property *pp = of_find_property(np, name, NULL);

	if (pp == NULL)
		return (ERR_PTR(-EINVAL));
	if (pp->value == NULL || pp->length == 0)
		return (ERR_PTR(-ENODATA));
	if ((u32)pp->length < min)
		return (ERR_PTR(-EOVERFLOW));
	if (max != 0 && (u32)pp->length > max)
		return (ERR_PTR(-EOVERFLOW));
	if (len != NULL)
		*len = pp->length;
	return (pp->value);
}

int
of_property_read_u32_index(const struct device_node *np, const char *name, u32 index, u32 *out)
{
	const __be32 *val = of_find_property_value_of_size(np, name, (index + 1) * 4, 0, NULL);

	if (IS_ERR(val))
		return (PTR_ERR(val));
	*out = be32_to_cpu(val[index]);
	return (0);
}

int
of_property_read_u64_index(const struct device_node *np, const char *name, u32 index, u64 *out)
{
	const __be32 *val = of_find_property_value_of_size(np, name, (index + 1) * 8, 0, NULL);

	if (IS_ERR(val))
		return (PTR_ERR(val));
	*out = of_read_number(val + index * 2, 2);
	return (0);
}

int
of_property_read_variable_u8_array(const struct device_node *np, const char *name, u8 *out,
    size_t sz_min, size_t sz_max)
{
	size_t sz, count;
	const u8 *val = of_find_property_value_of_size(np, name, sz_min, sz_max, &sz);

	if (IS_ERR(val))
		return (PTR_ERR(val));
	count = sz_max ? sz : sz_min;
	memcpy(out, val, count);
	return (count);
}

int
of_property_read_variable_u32_array(const struct device_node *np, const char *name, u32 *out,
    size_t sz_min, size_t sz_max)
{
	size_t sz, count, i;
	const __be32 *val = of_find_property_value_of_size(np, name, sz_min * 4, sz_max * 4, &sz);

	if (IS_ERR(val))
		return (PTR_ERR(val));
	count = sz_max ? sz / 4 : sz_min;
	for (i = 0; i < count; i++)
		out[i] = be32_to_cpu(val[i]);
	return (count);
}

int
of_property_read_variable_u64_array(const struct device_node *np, const char *name, u64 *out,
    size_t sz_min, size_t sz_max)
{
	size_t sz, count, i;
	const __be32 *val = of_find_property_value_of_size(np, name, sz_min * 8, sz_max * 8, &sz);

	if (IS_ERR(val))
		return (PTR_ERR(val));
	count = sz_max ? sz / 8 : sz_min;
	for (i = 0; i < count; i++)
		out[i] = of_read_number(val + 2 * i, 2);
	return (count);
}

int
of_property_count_elems_of_size(const struct device_node *np, const char *name, int elem_size)
{
	struct property *pp = of_find_property(np, name, NULL);

	if (pp == NULL)
		return (-EINVAL);
	if (pp->length % elem_size != 0)
		return (-EINVAL);
	return (pp->length / elem_size);
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	struct property *pp = of_find_property(np, name, NULL);

	if (pp == NULL)
		return (-EINVAL);
	if (pp->length == 0)
		return (-ENODATA);
	if (strnlen(pp->value, pp->length) >= (size_t)pp->length)
		return (-EILSEQ);
	*out = pp->value;
	return (0);
}

int
of_property_read_string_helper(const struct device_node *np, const char *name, const char **out,
    size_t sz, int skip)
{
	struct property *pp = of_find_property(np, name, NULL);
	const char *p, *end;
	int i = 0;
	size_t l;

	if (pp == NULL)
		return (-EINVAL);
	if (pp->value == NULL)
		return (-ENODATA);
	p = pp->value;
	end = p + pp->length;
	for (i = 0; p < end && (out == NULL || (size_t)i < skip + sz); i++, p += l) {
		l = strnlen(p, end - p) + 1;
		if (p + l > end)
			return (-EILSEQ);
		if (out != NULL && i >= skip)
			*out++ = p;
	}
	i -= skip;
	return (i <= 0 ? -ENODATA : i);
}

int
of_property_match_string(const struct device_node *np, const char *name, const char *string)
{
	struct property *pp = of_find_property(np, name, NULL);
	const char *p, *end;
	size_t l;
	int i;

	if (pp == NULL)
		return (-EINVAL);
	if (pp->value == NULL)
		return (-ENODATA);
	p = pp->value;
	end = p + pp->length;
	for (i = 0; p < end; i++, p += l) {
		l = strnlen(p, end - p) + 1;
		if (p + l > end)
			return (-EILSEQ);
		if (strcmp(string, p) == 0)
			return (i);
	}
	return (-ENODATA);
}

const __be32 *
of_prop_next_u32(struct property *prop, const __be32 *cur, u32 *pu)
{
	const void *curv = cur;

	if (prop == NULL)
		return (NULL);
	if (cur == NULL) {
		curv = prop->value;
		goto out_val;
	}
	curv = (const char *)curv + sizeof(*cur);
	if ((const char *)curv >= (const char *)prop->value + prop->length)
		return (NULL);
out_val:
	if (prop->length < 4)
		return (NULL);
	*pu = be32_to_cpup(curv);
	return (curv);
}

const char *
of_prop_next_string(struct property *prop, const char *cur)
{
	const void *curv = cur;

	if (prop == NULL)
		return (NULL);
	if (cur == NULL)
		return (prop->length > 0 ? prop->value : NULL);
	curv = (const char *)curv + strlen(cur) + 1;
	if ((const char *)curv >= (const char *)prop->value + prop->length)
		return (NULL);
	return (curv);
}

/* ---- phandles ---- */

static int
of_parse_phandle_common(const struct device_node *np, const char *list_name,
    const char *cells_name, int cell_count, int index, struct of_phandle_args *out)
{
	struct property *pp = of_find_property(np, list_name, NULL);
	const __be32 *list, *end;
	int cur = 0;

	if (pp == NULL)
		return (-ENOENT);
	list = pp->value;
	end = list + pp->length / 4;
	while (list < end) {
		phandle_t ph = be32_to_cpup(list++);
		struct device_node *target = NULL;
		u32 count = 0;

		if (ph != 0) {
			target = of_find_node_by_phandle(ph);
			if (target == NULL)
				return (-EINVAL);
			if (cells_name != NULL) {
				if (of_property_read_u32(target, cells_name, &count) != 0) {
					if (cell_count >= 0)
						count = cell_count;
					else
						return (-EINVAL);
				}
			} else
				count = cell_count;
			if (list + count > end)
				return (-EINVAL);
		}
		if (cur == index) {
			if (ph == 0)
				return (-ENOENT);
			if (out != NULL) {
				u32 i;

				if (count > MAX_PHANDLE_ARGS)
					count = MAX_PHANDLE_ARGS;
				out->np = target;
				out->args_count = count;
				for (i = 0; i < count; i++)
					out->args[i] = be32_to_cpup(list + i);
			}
			return (0);
		}
		list += count;
		cur++;
	}
	return (index < 0 ? cur : -ENOENT);
}

struct device_node *
of_parse_phandle(const struct device_node *np, const char *name, int index)
{
	struct of_phandle_args args;

	if (index < 0 || of_parse_phandle_common(np, name, NULL, 0, index, &args) != 0)
		return (NULL);
	return (args.np);
}

int
of_parse_phandle_with_args(const struct device_node *np, const char *list_name,
    const char *cells_name, int index, struct of_phandle_args *out)
{
	if (index < 0)
		return (-EINVAL);
	return (of_parse_phandle_common(np, list_name, cells_name, -1, index, out));
}

int
of_parse_phandle_with_fixed_args(const struct device_node *np, const char *list_name,
    int cell_count, int index, struct of_phandle_args *out)
{
	if (index < 0)
		return (-EINVAL);
	return (of_parse_phandle_common(np, list_name, NULL, cell_count, index, out));
}

int
of_count_phandle_with_args(const struct device_node *np, const char *list_name,
    const char *cells_name)
{
	if (cells_name == NULL) {
		struct property *pp = of_find_property(np, list_name, NULL);

		return (pp == NULL ? -ENOENT : pp->length / 4);
	}
	return (of_parse_phandle_common(np, list_name, cells_name, -1, -1, NULL));
}

int
of_n_addr_cells(struct device_node *np)
{
	u32 cells;

	for (np = np != NULL ? np->parent : NULL; np != NULL; np = np->parent)
		if (of_property_read_u32(np, "#address-cells", &cells) == 0)
			return (cells);
	return (2);
}

int
of_n_size_cells(struct device_node *np)
{
	u32 cells;

	for (np = np != NULL ? np->parent : NULL; np != NULL; np = np->parent)
		if (of_property_read_u32(np, "#size-cells", &cells) == 0)
			return (cells);
	return (1);
}

/* ---- addresses ---- */

/* of_bus_n_*_cells(): start at @np itself (the cells that describe its children). */
static int
of_bus_n_cells(struct device_node *np, const char *prop, int dflt)
{
	u32 cells;

	for (; np != NULL; np = np->parent)
		if (of_property_read_u32(np, prop, &cells) == 0)
			return (cells);
	return (dflt);
}

/*
 * Linux of/address.c: one (address, size) tuple of an "iommu-addresses" style property,
 * refusing a truncated one. DMA addresses are not translated here (no dma-ranges in play).
 */
const __be32 *
of_translate_dma_region_checked(struct device_node *dev, const __be32 *prop, size_t cells,
    phys_addr_t *start, size_t *length)
{
	int na = of_bus_n_cells(dev, "#address-cells", 2);
	int ns = of_bus_n_cells(dev, "#size-cells", 1);

	if (cells < (size_t)(na + ns))
		return (NULL);
	*start = of_read_number(prop, na);
	*length = of_read_number(prop + na, ns);
	return (prop + na + ns);
}

int
of_address_to_resource(struct device_node *np, int index, struct resource *r)
{
	bus_addr_t pa;
	bus_size_t size;
	const char *name = NULL;

	if (np == NULL || ofw_reg_to_paddr(np->node, index, &pa, &size, NULL) != 0)
		return (-EINVAL);
	of_property_read_string_index(np, "reg-names", index, &name);
	memset(r, 0, sizeof(*r));
	r->start = pa;
	r->end = pa + size - 1;
	r->flags = IORESOURCE_MEM;
	r->name = name != NULL ? name : np->full_name;
	return (0);
}

void __iomem *
of_iomap(struct device_node *np, int index)
{
	struct resource r;

	if (of_address_to_resource(np, index, &r) != 0)
		return (NULL);
	return ((void __iomem *)pmap_mapdev(r.start, resource_size(&r)));
}

/* ---- /reserved-memory ---- */

struct reserved_mem *
of_reserved_mem_lookup(struct device_node *np)
{
	struct appledrm_node *an;
	struct reserved_mem *rm;
	const __be32 *reg;
	int len, ac, sc;

	if (np == NULL)
		return (NULL);
	an = container_of(np, struct appledrm_node, np);
	if (an->rmem != NULL)
		return (an->rmem);
	reg = of_get_property(np, "reg", &len);
	if (reg == NULL)
		return (NULL);
	ac = of_n_addr_cells(np);
	sc = of_n_size_cells(np);
	if (len < (ac + sc) * 4)
		return (NULL);
	rm = kzalloc(sizeof(*rm), GFP_KERNEL);
	rm->name = np->full_name;
	rm->base = of_read_number(reg, ac);
	rm->size = of_read_number(reg + ac, sc);
	an->rmem = rm;
	return (rm);
}

/* ---- graph ---- */

struct device_node *
of_graph_get_next_port(const struct device_node *parent, struct device_node *prev)
{
	struct device_node *ports;

	if (parent == NULL)
		return (NULL);
	if (prev == NULL) {
		ports = of_get_child_by_name(parent, "ports");
		if (ports != NULL)
			parent = ports;
	} else
		parent = prev->parent;
	do {
		prev = of_get_next_child(parent, prev);
	} while (prev != NULL && !of_node_name_eq(prev, "port"));
	return (prev);
}

struct device_node *
of_graph_get_next_port_endpoint(const struct device_node *port, struct device_node *prev)
{
	do {
		prev = of_get_next_child(port, prev);
	} while (prev != NULL && !of_node_name_eq(prev, "endpoint"));
	return (prev);
}

struct device_node *
of_graph_get_next_endpoint(const struct device_node *parent, struct device_node *prev)
{
	struct device_node *port, *ep;

	port = prev != NULL ? prev->parent : of_graph_get_next_port(parent, NULL);
	ep = prev;
	while (port != NULL) {
		ep = of_graph_get_next_port_endpoint(port, ep);
		if (ep != NULL)
			return (ep);
		port = of_graph_get_next_port(parent, port);
		ep = NULL;
	}
	return (NULL);
}

struct device_node *
of_graph_get_port_by_id(struct device_node *node, u32 id)
{
	struct device_node *port;

	for (port = of_graph_get_next_port(node, NULL); port != NULL;
	    port = of_graph_get_next_port(node, port)) {
		u32 reg = 0;

		of_property_read_u32(port, "reg", &reg);
		if (reg == id)
			return (port);
	}
	return (NULL);
}

struct device_node *
of_graph_get_endpoint_by_regs(const struct device_node *parent, int port_reg, int reg)
{
	struct device_node *ep = NULL;

	while ((ep = of_graph_get_next_endpoint(parent, ep)) != NULL) {
		u32 p = 0, e = 0;

		of_property_read_u32(ep->parent, "reg", &p);
		of_property_read_u32(ep, "reg", &e);
		if ((port_reg == -1 || (u32)port_reg == p) && (reg == -1 || (u32)reg == e))
			return (ep);
	}
	return (NULL);
}

struct device_node *
of_graph_get_remote_endpoint(const struct device_node *node)
{
	return (of_parse_phandle(node, "remote-endpoint", 0));
}

struct device_node *
of_graph_get_remote_port_parent(const struct device_node *node)
{
	struct device_node *np = of_graph_get_remote_endpoint(node);
	int depth;

	/* endpoint -> port -> (ports ->) device */
	for (depth = 3; np != NULL && depth > 0; depth--) {
		np = np->parent;
		if (np != NULL && depth == 2 && !of_node_name_eq(np, "ports"))
			break;
	}
	return (np);
}

/* ---- of_changeset: edits to the property cache (the FDT itself is never written) ---- */

struct of_changeset_entry {
	struct list_head	node;
	struct device_node	*np;
	char			*name;
	void			*value, *old_value;
	int			length, old_length;
	bool			applied;
};

void
of_changeset_init(struct of_changeset *ocs)
{
	INIT_LIST_HEAD(&ocs->entries);
}

void
of_changeset_destroy(struct of_changeset *ocs)
{
	struct of_changeset_entry *ce, *n;

	list_for_each_entry_safe(ce, n, &ocs->entries, node) {
		list_del(&ce->node);
		kfree(ce->name);
		kfree(ce);
	}
}

int
of_changeset_update_prop_string(struct of_changeset *ocs, struct device_node *np,
    const char *prop_name, const char *str)
{
	struct of_changeset_entry *ce = kzalloc(sizeof(*ce), GFP_KERNEL);

	ce->np = np;
	ce->name = appledrm_strdup(prop_name);
	ce->value = appledrm_strdup(str);
	ce->length = strlen(str) + 1;
	list_add_tail(&ce->node, &ocs->entries);
	return (0);
}

static struct property *
of_cache_property(struct device_node *np, const char *name)
{
	struct appledrm_node *an = container_of(np, struct appledrm_node, np);
	struct property *pp = of_find_property(np, name, NULL);

	if (pp == NULL) {
		pp = kzalloc(sizeof(*pp), GFP_KERNEL);
		pp->name = appledrm_strdup(name);
		pp->length = -1;	/* marks "absent before the changeset" */
		mtx_lock(&node_mtx);
		pp->next = an->props;
		an->props = pp;
		mtx_unlock(&node_mtx);
	}
	return (pp);
}

int
of_changeset_apply(struct of_changeset *ocs)
{
	struct of_changeset_entry *ce;

	list_for_each_entry(ce, &ocs->entries, node) {
		struct property *pp = of_cache_property(ce->np, ce->name);

		ce->old_value = pp->value;
		ce->old_length = pp->length;
		pp->value = ce->value;
		pp->length = ce->length;
		ce->applied = true;
	}
	return (0);
}

int
of_changeset_revert(struct of_changeset *ocs)
{
	struct of_changeset_entry *ce;

	list_for_each_entry_reverse(ce, &ocs->entries, node) {
		struct property *pp;

		if (!ce->applied)
			continue;
		pp = of_cache_property(ce->np, ce->name);
		pp->value = ce->old_value;
		pp->length = ce->old_length;
		ce->applied = false;
	}
	return (0);
}
