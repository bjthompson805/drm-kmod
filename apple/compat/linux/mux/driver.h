/* Mux provider API (drivers/mux): dcp-fabric registers the display crossbar mux. */
#ifndef _APPLEDRM_LINUX_MUX_DRIVER_H_
#define _APPLEDRM_LINUX_MUX_DRIVER_H_
#include <linux/device.h>
#include <linux/mux/consumer.h>
#define MUX_IDLE_AS_IS		(-1)
#define MUX_IDLE_DISCONNECT	(-2)
struct mux_chip;
struct mux_control_ops {
	int (*set)(struct mux_control *mux, int state);
};
struct mux_control {
	struct mux_chip	*chip;
	int		cached_state;
	unsigned int	states;
	int		idle_state;
};
struct mux_chip {
	unsigned int			controllers;
	struct mux_control		*mux;
	struct device			dev;
	int				id;
	const struct mux_control_ops	*ops;
};
static inline unsigned int mux_control_states(struct mux_control *mux) { return mux->states; }
#endif
