/* mux control: used by drm/apple only for external DP crossbars; not wired on FreeBSD yet. */
#ifndef _APPLEDRM_LINUX_MUX_CONSUMER_H_
#define _APPLEDRM_LINUX_MUX_CONSUMER_H_
#include <linux/err.h>
struct device;
struct mux_control;
struct mux_state;
static inline struct mux_control *mux_control_get(struct device *dev, const char *name)
{ return ERR_PTR(-ENODEV); }
static inline struct mux_control *devm_mux_control_get(struct device *dev, const char *name)
{ return ERR_PTR(-ENODEV); }
static inline void mux_control_put(struct mux_control *mux) { }
static inline int mux_control_select(struct mux_control *mux, unsigned int state) { return -ENODEV; }
static inline int mux_control_try_select(struct mux_control *mux, unsigned int state) { return -ENODEV; }
static inline int mux_control_deselect(struct mux_control *mux) { return 0; }
#endif
