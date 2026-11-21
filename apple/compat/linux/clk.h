/* Minimal clk API for drm/apple: rates come from the DT "clocks" fixed-clock nodes (appledrm_compat.c). */
#ifndef _APPLEDRM_LINUX_CLK_H_
#define _APPLEDRM_LINUX_CLK_H_
#include <linux/device.h>
#include <linux/err.h>
struct clk;
struct clk_bulk_data { const char *id; struct clk *clk; };
struct clk *appledrm_clk_get(struct device *dev, const char *id, bool optional);
unsigned long clk_get_rate(struct clk *clk);
void clk_put(struct clk *clk);
#define clk_get(dev, id)		appledrm_clk_get(dev, id, false)
#define clk_get_optional(dev, id)	appledrm_clk_get(dev, id, true)
#define devm_clk_get(dev, id)		appledrm_clk_get(dev, id, false)
#define devm_clk_get_optional(dev, id)	appledrm_clk_get(dev, id, true)
void devm_clk_put(struct device *dev, struct clk *clk);
int devm_clk_bulk_get(struct device *dev, int num, struct clk_bulk_data *clks);
int devm_clk_bulk_get_all(struct device *dev, struct clk_bulk_data **clks);
static inline int clk_bulk_prepare_enable(int n, const struct clk_bulk_data *c) { return 0; }
static inline void clk_bulk_disable_unprepare(int n, const struct clk_bulk_data *c) { }
static inline int clk_prepare_enable(struct clk *c) { return 0; }
static inline void clk_disable_unprepare(struct clk *c) { }
#endif
