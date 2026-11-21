/*
 * Small Linux subsystems drm/apple needs on FreeBSD: clocks (fixed-clock only), the
 * backlight class (as sysctl hw.appledrm.<name>), MMIO regmap and syscon, RAM queries,
 * memremap, and DRM helpers drm-kmod 6.12 lacks.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/backlight.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_simple_kms_helper.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/vmparam.h>
#include <vm/vm_dumpset.h>

#include "dcp.h"
#include "dcpext_drm.h"

SYSCTL_NODE(_hw, OID_AUTO, appledrm, CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "Apple DCP display");

/* ---- clocks: DT "clocks"/"clock-names" to fixed-clock rates ---- */

struct clk {
	unsigned long	rate;
	const char	*name;
};

static struct clk *
appledrm_clk_index(struct device *dev, int index)
{
	struct of_phandle_args args;
	struct clk *clk;
	u32 rate = 0;

	if (of_parse_phandle_with_args(dev->of_node, "clocks", "#clock-cells", index, &args) != 0)
		return (NULL);
	clk = kzalloc(sizeof(*clk), GFP_KERNEL);
	clk->name = args.np->full_name;
	if (of_device_is_compatible(args.np, "fixed-clock"))
		of_property_read_u32(args.np, "clock-frequency", &rate);
	else
		dev_warn(dev, "clock %s is not a fixed-clock; rate 0\n", args.np->full_name);
	clk->rate = rate;
	return (clk);
}

struct clk *
appledrm_clk_get(struct device *dev, const char *id, bool optional)
{
	struct clk *clk;
	int index = 0;

	if (id != NULL) {
		index = of_property_match_string(dev->of_node, "clock-names", id);
		if (index < 0)
			return (optional ? NULL : ERR_PTR(-ENOENT));
	}
	clk = appledrm_clk_index(dev, index);
	if (clk == NULL)
		return (optional ? NULL : ERR_PTR(-ENOENT));
	return (clk);
}

unsigned long
clk_get_rate(struct clk *clk)
{
	return (clk != NULL && !IS_ERR(clk) ? clk->rate : 0);
}

void
clk_put(struct clk *clk)
{
}

void
devm_clk_put(struct device *dev, struct clk *clk)
{
}

int
devm_clk_bulk_get(struct device *dev, int num, struct clk_bulk_data *clks)
{
	int i;

	for (i = 0; i < num; i++) {
		clks[i].clk = appledrm_clk_get(dev, clks[i].id, false);
		if (IS_ERR(clks[i].clk))
			return (PTR_ERR(clks[i].clk));
	}
	return (0);
}

int
devm_clk_bulk_get_all(struct device *dev, struct clk_bulk_data **clksp)
{
	struct clk_bulk_data *clks;
	int i, n = of_count_phandle_with_args(dev->of_node, "clocks", "#clock-cells");

	if (n <= 0) {
		*clksp = NULL;
		return (0);
	}
	clks = kcalloc(n, sizeof(*clks), GFP_KERNEL);
	for (i = 0; i < n; i++) {
		of_property_read_string_index(dev->of_node, "clock-names", i, &clks[i].id);
		clks[i].clk = appledrm_clk_index(dev, i);
	}
	*clksp = clks;
	return (n);
}

/* ---- backlight ---- */

static int
appledrm_bl_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct backlight_device *bd = arg1;
	int val = bd->props.brightness, error;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val < 0 || val > bd->props.max_brightness)
		return (EINVAL);
	mutex_lock(&bd->ops_lock);
	bd->props.brightness = val;
	error = -backlight_update_status(bd);
	mutex_unlock(&bd->ops_lock);
	return (error);
}

struct backlight_device *
backlight_device_register(const char *name, struct device *parent, void *devdata,
    const struct backlight_ops *ops, const struct backlight_properties *props)
{
	struct backlight_device *bd = kzalloc(sizeof(*bd), GFP_KERNEL);
	struct sysctl_oid *node;
	struct sysctl_ctx_list *ctx;
	char sname[64];
	char *c;

	mutex_init(&bd->update_lock);
	mutex_init(&bd->ops_lock);
	bd->ops = ops;
	bd->data = devdata;
	bd->parent = parent;
	if (props != NULL)
		bd->props = *props;
	bd->name = kstrdup(name, GFP_KERNEL);
	strlcpy(sname, name, sizeof(sname));
	for (c = sname; *c; c++)
		if (*c == '-' || *c == '.')
			*c = '_';
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	sysctl_ctx_init(ctx);
	bd->priv = ctx;
	node = SYSCTL_ADD_NODE(ctx, SYSCTL_STATIC_CHILDREN(_hw_appledrm), OID_AUTO, sname,
	    CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, "backlight");
	SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(node), OID_AUTO, "brightness",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, bd, 0, appledrm_bl_sysctl, "I",
	    "backlight brightness");
	SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(node), OID_AUTO, "max_brightness", CTLFLAG_RD,
	    &bd->props.max_brightness, 0, "maximum brightness");
	return (bd);
}

void
backlight_device_unregister(struct backlight_device *bd)
{
	if (bd == NULL)
		return;
	sysctl_ctx_free(bd->priv);
	kfree(bd->priv);
	kfree(bd->name);
	kfree(bd);
}

static void
devm_backlight_release(void *bd)
{
	backlight_device_unregister(bd);
}

struct backlight_device *
devm_backlight_device_register(struct device *dev, const char *name, struct device *parent,
    void *devdata, const struct backlight_ops *ops, const struct backlight_properties *props)
{
	struct backlight_device *bd = backlight_device_register(name, parent, devdata, ops, props);

	devm_add_action(dev, devm_backlight_release, bd);
	return (bd);
}

void
devm_backlight_device_unregister(struct device *dev, struct backlight_device *bd)
{
}

/* ---- regmap (MMIO, 32-bit) and syscon ---- */

struct regmap {
	void __iomem	*base;
	unsigned int	stride;
	struct mutex	lock;
};

struct regmap *
regmap_init_mmio(struct device *dev, void __iomem *regs, const struct regmap_config *cfg)
{
	struct regmap *map = kzalloc(sizeof(*map), GFP_KERNEL);

	map->base = regs;
	map->stride = cfg != NULL && cfg->reg_stride ? cfg->reg_stride : 4;
	mutex_init(&map->lock);
	return (map);
}

void
regmap_exit(struct regmap *map)
{
	kfree(map);
}

int
regmap_read(struct regmap *map, unsigned int reg, unsigned int *val)
{
	*val = readl((char __iomem *)map->base + reg);
	return (0);
}

int
regmap_write(struct regmap *map, unsigned int reg, unsigned int val)
{
	writel(val, (char __iomem *)map->base + reg);
	return (0);
}

int
regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask, unsigned int val)
{
	unsigned int v;

	mutex_lock(&map->lock);
	v = readl((char __iomem *)map->base + reg);
	writel((v & ~mask) | (val & mask), (char __iomem *)map->base + reg);
	mutex_unlock(&map->lock);
	return (0);
}

struct syscon {
	struct list_head	list;
	struct device_node	*np;
	struct regmap		*map;
};
static LIST_HEAD(syscons);
static DEFINE_MUTEX(syscon_lock);

struct regmap *
syscon_node_to_regmap(struct device_node *np)
{
	struct syscon *s;
	void __iomem *base;
	struct regmap_config cfg = { .reg_stride = 4 };

	if (np == NULL)
		return (ERR_PTR(-EINVAL));
	mutex_lock(&syscon_lock);
	list_for_each_entry(s, &syscons, list)
		if (s->np == np) {
			mutex_unlock(&syscon_lock);
			return (s->map);
		}
	base = of_iomap(np, 0);
	if (base == NULL) {
		mutex_unlock(&syscon_lock);
		return (ERR_PTR(-ENOMEM));
	}
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	s->np = np;
	s->map = regmap_init_mmio(NULL, base, &cfg);
	list_add(&s->list, &syscons);
	mutex_unlock(&syscon_lock);
	return (s->map);
}

struct regmap *
syscon_regmap_lookup_by_compatible(const char *compat)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, compat);

	return (np != NULL ? syscon_node_to_regmap(np) : ERR_PTR(-ENODEV));
}

struct regmap *
syscon_regmap_lookup_by_phandle_args(struct device_node *np, const char *property,
    int arg_count, unsigned int *out_args)
{
	struct of_phandle_args args;
	int i;

	if (of_parse_phandle_with_fixed_args(np, property, arg_count, 0, &args) != 0)
		return (ERR_PTR(-ENODEV));
	for (i = 0; i < arg_count && i < args.args_count; i++)
		out_args[i] = args.args[i];
	return (syscon_node_to_regmap(args.np));
}

/* ---- RAM queries ---- */

bool
appledrm_pa_is_ram(vm_paddr_t pa)
{
	int i;

	for (i = 0; dump_avail[i + 1] != 0; i += 2)
		if (pa >= dump_avail[i] && pa < dump_avail[i + 1])
			return (true);
	return (false);
}

int
region_intersects(resource_size_t start, size_t size, unsigned long flags, unsigned long desc)
{
	vm_paddr_t pa;
	bool any = false, all = true;

	for (pa = trunc_page(start); pa < start + size; pa += PAGE_SIZE) {
		if (appledrm_pa_is_ram(pa))
			any = true;
		else
			all = false;
	}
	return (all ? REGION_INTERSECTS : any ? REGION_MIXED : REGION_DISJOINT);
}

static struct resource appledrm_dummy_region;

struct resource *
appledrm_request_mem_region(resource_size_t start, resource_size_t n, const char *name)
{
	return (&appledrm_dummy_region);
}

void
appledrm_release_mem_region(resource_size_t start, resource_size_t n)
{
}

struct resource *
devm_request_mem_region(struct device *dev, resource_size_t start, resource_size_t n,
    const char *name)
{
	return (&appledrm_dummy_region);
}

void *
devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags)
{
	if (appledrm_pa_is_ram(offset) && appledrm_pa_is_ram(offset + size - 1))
		return ((void *)PHYS_TO_DMAP(offset));
	return (pmap_mapdev_attr(offset, size,
	    (flags & MEMREMAP_WC) ? VM_MEMATTR_WRITE_COMBINING : VM_MEMATTR_DEFAULT));
}

void
devm_memunmap(struct device *dev, void *addr)
{
}

/* ---- DRM helpers missing from drm-kmod 6.12 ---- */

static const struct drm_encoder_funcs drm_simple_encoder_funcs_cleanup = {
	.destroy = drm_encoder_cleanup,
};

int
drm_simple_encoder_init(struct drm_device *dev, struct drm_encoder *encoder, int encoder_type)
{
	return (drm_encoder_init(dev, encoder, &drm_simple_encoder_funcs_cleanup, encoder_type,
	    NULL));
}

/* Linux 6.13 drm_atomic_helper.c */
int
drm_atomic_helper_reset_crtc(struct drm_crtc *crtc, struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_atomic_state *state;
	struct drm_crtc_state *crtc_state;
	int ret;

	state = drm_atomic_state_alloc(crtc->dev);
	if (!state)
		return (-ENOMEM);
	state->acquire_ctx = ctx;
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}
	crtc_state->connectors_changed = true;
	ret = drm_atomic_commit(state);
out:
	drm_atomic_state_put(state);
	return (ret);
}

/* ---- j613_dcp_activate.c (M3 MacBook Air, 25G83) is not built: not this machine ---- */

unsigned int
apple_j613_25g83_clock_hz(void)
{
	return (0);
}

int
apple_j613_25g83_coldplug(void)
{
	return (0);
}

bool
apple_t8122_25g83_board(void)
{
	return (false);
}

/* ---- dcpext_drm.c (external-display DRM device, shmem GEM) is not built yet ---- */

int
dcpext_drm_register(struct apple_dcp *dcp, void *pixels, size_t size, u32 stride,
    atomic_t *terminal_error, struct drm_device **retained_drm)
{
	return (-ENODEV);
}

/* ---- module params (kernel/params.c) ---- */

int
param_set_int(const char *val, const struct kernel_param *kp)
{
	long v;

	if (val == NULL || kstrtol(val, 0, &v) != 0)
		return (-EINVAL);
	*(int *)kp->arg = (int)v;
	return (0);
}

int
param_get_int(char *buffer, const struct kernel_param *kp)
{
	return (snprintf(buffer, 4096, "%d\n", *(int *)kp->arg));
}
