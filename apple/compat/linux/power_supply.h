/* power_supply class: absent on FreeBSD; drm/apple reads battery temperature only as a hint. */
#ifndef _APPLEDRM_LINUX_POWER_SUPPLY_H_
#define _APPLEDRM_LINUX_POWER_SUPPLY_H_
#include <linux/types.h>
#include <linux/errno.h>
enum power_supply_type { POWER_SUPPLY_TYPE_UNKNOWN = 0, POWER_SUPPLY_TYPE_BATTERY };
enum power_supply_scope { POWER_SUPPLY_SCOPE_UNKNOWN = 0, POWER_SUPPLY_SCOPE_SYSTEM, POWER_SUPPLY_SCOPE_DEVICE };
enum power_supply_property { POWER_SUPPLY_PROP_TEMP = 1, POWER_SUPPLY_PROP_SCOPE };
union power_supply_propval { int intval; const char *strval; };
struct power_supply_desc { const char *name; enum power_supply_type type; };
struct power_supply { const struct power_supply_desc *desc; };
static inline int power_supply_get_property(struct power_supply *psy, enum power_supply_property p,
    union power_supply_propval *v) { return -ENODEV; }
static inline int power_supply_for_each_psy(void *data, int (*fn)(struct power_supply *, void *))
{ return 0; }
#endif
