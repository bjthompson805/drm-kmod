/*
 * Linux component framework (drivers/base/component.c semantics, reduced): an aggregate
 * (master) binds once every match entry has an added component; component_bind_all()
 * binds the components in match order.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/component.h>
#include <linux/of.h>
#include <drm/drm_of.h>

struct component;

struct component_match_entry {
	int		(*compare)(struct device *, void *);
	void		(*release)(struct device *, void *);
	void		*data;
	struct component *component;
};

struct component_match {
	size_t				num;
	struct component_match_entry	ent[16];
};

struct aggregate_device {
	struct list_head		list;
	const struct component_master_ops *ops;
	struct device			*parent;
	struct component_match		*match;
	bool				bound;
};

struct component {
	struct list_head		list;
	struct aggregate_device		*adev;
	bool				bound;
	const struct component_ops	*ops;
	struct device			*dev;
};

static DEFINE_MUTEX(component_mutex);
static LIST_HEAD(component_list);
static LIST_HEAD(aggregate_devices);

int
component_compare_of(struct device *dev, void *data)
{
	return (dev->of_node == data);
}

void
component_match_add_release(struct device *parent, struct component_match **matchptr,
    void (*release)(struct device *, void *), int (*compare)(struct device *, void *),
    void *compare_data)
{
	struct component_match *m = *matchptr;

	if (IS_ERR(m))
		return;
	if (m == NULL) {
		m = kzalloc(sizeof(*m), GFP_KERNEL);
		*matchptr = m;
	}
	if (m->num == nitems(m->ent)) {
		*matchptr = ERR_PTR(-ENOMEM);
		return;
	}
	m->ent[m->num].compare = compare;
	m->ent[m->num].release = release;
	m->ent[m->num].data = compare_data;
	m->num++;
}

void
drm_of_component_match_add(struct device *master, struct component_match **matchptr,
    int (*compare)(struct device *, void *), struct device_node *node)
{
	component_match_add_release(master, matchptr, NULL, compare, of_node_get(node));
}

static int
find_components(struct aggregate_device *adev)
{
	struct component_match *m = adev->match;
	size_t i;

	for (i = 0; i < m->num; i++) {
		struct component *c;

		if (m->ent[i].component != NULL)
			continue;
		list_for_each_entry(c, &component_list, list) {
			if (c->adev != NULL && c->adev != adev)
				continue;
			if (m->ent[i].compare(c->dev, m->ent[i].data)) {
				m->ent[i].component = c;
				c->adev = adev;
				break;
			}
		}
		if (m->ent[i].component == NULL)
			return (-ENXIO);
	}
	return (0);
}

static int
try_to_bring_up_aggregate(struct aggregate_device *adev)
{
	int ret;

	if (adev->bound || find_components(adev) != 0)
		return (0);
	ret = adev->ops->bind(adev->parent);
	if (ret == 0)
		adev->bound = true;
	else if (ret != -EPROBE_DEFER)
		dev_err(adev->parent, "aggregate bind failed: %d\n", ret);
	return (ret);
}

static void
try_all(void)
{
	struct aggregate_device *adev;

	list_for_each_entry(adev, &aggregate_devices, list)
		try_to_bring_up_aggregate(adev);
}

int
component_master_add_with_match(struct device *parent, const struct component_master_ops *ops,
    struct component_match *match)
{
	struct aggregate_device *adev;
	int ret;

	if (IS_ERR(match))
		return (PTR_ERR(match));
	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	adev->parent = parent;
	adev->ops = ops;
	adev->match = match;
	mutex_lock(&component_mutex);
	list_add(&adev->list, &aggregate_devices);
	ret = try_to_bring_up_aggregate(adev);
	mutex_unlock(&component_mutex);
	/* Linux: a master whose components are missing waits, it is not an error. */
	return (ret == -EPROBE_DEFER ? 0 : ret);
}

void
component_master_del(struct device *parent, const struct component_master_ops *ops)
{
	struct aggregate_device *adev, *n;
	size_t i;

	mutex_lock(&component_mutex);
	list_for_each_entry_safe(adev, n, &aggregate_devices, list) {
		if (adev->parent != parent || adev->ops != ops)
			continue;
		if (adev->bound)
			ops->unbind(parent);
		for (i = 0; i < adev->match->num; i++)
			if (adev->match->ent[i].component != NULL)
				adev->match->ent[i].component->adev = NULL;
		list_del(&adev->list);
		kfree(adev->match);
		kfree(adev);
	}
	mutex_unlock(&component_mutex);
}

int
component_bind_all(struct device *parent, void *data)
{
	struct aggregate_device *adev = NULL, *a;
	size_t i;
	int ret = 0;

	list_for_each_entry(a, &aggregate_devices, list)
		if (a->parent == parent)
			adev = a;
	if (adev == NULL)
		return (-EINVAL);
	for (i = 0; i < adev->match->num; i++) {
		struct component *c = adev->match->ent[i].component;

		if (c == NULL || c->bound)
			continue;
		ret = c->ops->bind(c->dev, parent, data);
		if (ret != 0) {
			if (ret != -EPROBE_DEFER)
				dev_err(parent, "failed to bind %s: %d\n",
				    c->dev->of_node ? c->dev->of_node->full_name : "?", ret);
			while (i-- > 0) {
				c = adev->match->ent[i].component;
				if (c != NULL && c->bound) {
					c->ops->unbind(c->dev, parent, data);
					c->bound = false;
				}
			}
			return (ret);
		}
		c->bound = true;
	}
	return (0);
}

void
component_unbind_all(struct device *parent, void *data)
{
	struct aggregate_device *adev = NULL, *a;
	size_t i;

	list_for_each_entry(a, &aggregate_devices, list)
		if (a->parent == parent)
			adev = a;
	if (adev == NULL)
		return;
	for (i = adev->match->num; i-- > 0;) {
		struct component *c = adev->match->ent[i].component;

		if (c != NULL && c->bound) {
			c->ops->unbind(c->dev, parent, data);
			c->bound = false;
		}
	}
}

int
component_add(struct device *dev, const struct component_ops *ops)
{
	struct component *c = kzalloc(sizeof(*c), GFP_KERNEL);

	c->ops = ops;
	c->dev = dev;
	mutex_lock(&component_mutex);
	list_add_tail(&c->list, &component_list);
	try_all();
	mutex_unlock(&component_mutex);
	return (0);
}

void
component_del(struct device *dev, const struct component_ops *ops)
{
	struct component *c, *n;

	mutex_lock(&component_mutex);
	list_for_each_entry_safe(c, n, &component_list, list) {
		if (c->dev != dev || c->ops != ops)
			continue;
		if (c->adev != NULL && c->adev->bound) {
			c->adev->ops->unbind(c->adev->parent);
			c->adev->bound = false;
		}
		if (c->adev != NULL) {
			size_t i;

			for (i = 0; i < c->adev->match->num; i++)
				if (c->adev->match->ent[i].component == c)
					c->adev->match->ent[i].component = NULL;
		}
		list_del(&c->list);
		kfree(c);
	}
	mutex_unlock(&component_mutex);
}
