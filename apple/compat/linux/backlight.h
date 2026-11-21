/*
 * Linux 7.x backlight class layout for drm/apple (shadows LinuxKPI's backlight.h, whose
 * struct backlight_device has no embedded device or ops_lock). Registration lives in
 * appledrm_misc.c and exposes brightness as sysctl hw.appledrm.<name>.brightness.
 */
#ifndef _APPLEDRM_LINUX_BACKLIGHT_H_
#define _APPLEDRM_LINUX_BACKLIGHT_H_

#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/notifier.h>

enum backlight_update_reason { BACKLIGHT_UPDATE_HOTKEY, BACKLIGHT_UPDATE_SYSFS };
enum backlight_type { BACKLIGHT_RAW = 1, BACKLIGHT_PLATFORM, BACKLIGHT_FIRMWARE, BACKLIGHT_TYPE_MAX };
enum backlight_scale { BACKLIGHT_SCALE_UNKNOWN = 0, BACKLIGHT_SCALE_LINEAR, BACKLIGHT_SCALE_NON_LINEAR };

#define BACKLIGHT_POWER_ON		(0)
#define BACKLIGHT_POWER_OFF		(4)
#define BACKLIGHT_POWER_REDUCED		(1)

#define BL_CORE_SUSPENDED	(1 << 0)
#define BL_CORE_FBBLANK		(1 << 1)
#define BL_CORE_SUSPENDRESUME	(1 << 0)

struct backlight_device;
struct device;

struct backlight_ops {
	unsigned int options;
	int (*update_status)(struct backlight_device *);
	int (*get_brightness)(struct backlight_device *);
	bool (*controls_device)(struct backlight_device *bd, struct device *dev);
};

struct backlight_properties {
	int brightness;
	int max_brightness;
	int power;
	enum backlight_type type;
	unsigned int state;
	enum backlight_scale scale;
};

struct backlight_device {
	struct backlight_properties props;
	struct mutex update_lock;
	struct mutex ops_lock;
	const struct backlight_ops *ops;
	struct { struct kobject kobj; } dev;	/* Linux embeds a struct device; only kobj is used */
	struct device *parent;
	void *data;
	char *name;
	void *priv;		/* appledrm_misc.c */
};

static inline void *bl_get_data(struct backlight_device *bd) { return bd->data; }

static inline int backlight_update_status(struct backlight_device *bd)
{
	int ret = -ENOENT;

	mutex_lock(&bd->update_lock);
	if (bd->ops && bd->ops->update_status)
		ret = bd->ops->update_status(bd);
	mutex_unlock(&bd->update_lock);
	return ret;
}

static inline bool backlight_is_blank(const struct backlight_device *bd)
{
	return bd->props.power != BACKLIGHT_POWER_ON ||
	    bd->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK);
}

static inline int backlight_get_brightness(const struct backlight_device *bd)
{
	return backlight_is_blank(bd) ? 0 : bd->props.brightness;
}

struct backlight_device *backlight_device_register(const char *name, struct device *parent,
    void *devdata, const struct backlight_ops *ops, const struct backlight_properties *props);
void backlight_device_unregister(struct backlight_device *bd);
struct backlight_device *devm_backlight_device_register(struct device *dev, const char *name,
    struct device *parent, void *devdata, const struct backlight_ops *ops,
    const struct backlight_properties *props);
void devm_backlight_device_unregister(struct device *dev, struct backlight_device *bd);
static inline void backlight_force_update(struct backlight_device *bd,
    enum backlight_update_reason reason) { }

#endif
