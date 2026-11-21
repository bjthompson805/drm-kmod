/* Type-C mux consumer/provider API: no Type-C class on FreeBSD; providers register locally. */
#ifndef _APPLEDRM_LINUX_USB_TYPEC_MUX_H_
#define _APPLEDRM_LINUX_USB_TYPEC_MUX_H_
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/usb/typec_altmode.h>
struct device;
struct fwnode_handle;
struct typec_mux;
struct typec_mux_dev;
struct typec_mux_state {
	struct typec_altmode	*alt;
	unsigned long		mode;
	void			*data;
};
typedef void (*typec_mux_notify_fn_t)(struct typec_mux_dev *mux, struct typec_mux_state *state);
typedef int (*typec_mux_set_fn_t)(struct typec_mux_dev *mux, struct typec_mux_state *state);
/* aurora's include/linux/usb/typec_mux.h */
struct typec_mux_desc {
	struct fwnode_handle	*fwnode;
	typec_mux_set_fn_t	set;
	const char		*name;
	void			*drvdata;
	typec_mux_set_fn_t	prepare;
	typec_mux_set_fn_t	finish;
	typec_mux_notify_fn_t	commit;
	typec_mux_notify_fn_t	abort;
};
struct typec_mux_dev {
	struct typec_mux_desc	desc;
};
static inline struct typec_mux *fwnode_typec_mux_get(struct fwnode_handle *fw) { return NULL; }
static inline void typec_mux_put(struct typec_mux *mux) { }
static inline int typec_mux_set(struct typec_mux *mux, struct typec_mux_state *st) { return 0; }
static inline struct typec_mux_dev *typec_mux_register(struct device *parent,
    const struct typec_mux_desc *desc)
{
	struct typec_mux_dev *m = kzalloc(sizeof(*m), GFP_KERNEL);

	if (m == NULL)
		return ERR_PTR(-ENOMEM);
	m->desc = *desc;
	return m;
}
static inline void typec_mux_unregister(struct typec_mux_dev *m) { kfree(m); }
static inline void *typec_mux_get_drvdata(struct typec_mux_dev *m) { return m->desc.drvdata; }
#endif
