/* Generic PHY API: drm/apple uses it only for the external DP/Type-C PHYs; optional gets return NULL. */
#ifndef _APPLEDRM_LINUX_PHY_PHY_H_
#define _APPLEDRM_LINUX_PHY_PHY_H_
#include <linux/err.h>
#include <linux/types.h>
struct device_node;
enum phy_mode { PHY_MODE_INVALID, PHY_MODE_DP = 19 };
struct phy_configure_opts_dp {
	unsigned int link_rate, lanes;
	unsigned int voltage[4], pre[4];
	u8 ssc:1, set_rate:1, set_lanes:1, set_voltages:1;
};
union phy_configure_opts { struct phy_configure_opts_dp dp; };
struct phy_ops;
#include <linux/device.h>
struct phy {
	struct device		dev;	/* dev.of_node is the PHY's node */
	int			id;
	const struct phy_ops	*ops;
};
static inline struct phy *devm_phy_optional_get(struct device *dev, const char *s) { return NULL; }
static inline struct phy *devm_phy_get(struct device *dev, const char *s) { return ERR_PTR(-ENODEV); }
static inline struct phy *of_phy_get(struct device_node *np, const char *s) { return ERR_PTR(-ENODEV); }
static inline void of_phy_put(struct phy *phy) { }
static inline int phy_init(struct phy *phy) { return phy ? -ENODEV : 0; }
static inline int phy_exit(struct phy *phy) { return 0; }
static inline int phy_power_on(struct phy *phy) { return phy ? -ENODEV : 0; }
static inline int phy_power_off(struct phy *phy) { return 0; }
static inline int phy_set_mode_ext(struct phy *phy, enum phy_mode m, int s) { return phy ? -ENODEV : 0; }
#define phy_set_mode(phy, mode) phy_set_mode_ext(phy, mode, 0)
static inline int phy_configure(struct phy *phy, union phy_configure_opts *o) { return phy ? -ENODEV : 0; }
static inline int phy_validate(struct phy *phy, enum phy_mode m, int s, union phy_configure_opts *o)
{ return phy ? -ENODEV : 0; }
static inline bool phy_managed_by_typec(struct phy *phy) { return false; }
#endif
