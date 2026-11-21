/* GPIO consumer API: drm/apple's GPIOs (DP HPD) are optional; not wired on FreeBSD yet. */
#ifndef _APPLEDRM_LINUX_GPIO_CONSUMER_H_
#define _APPLEDRM_LINUX_GPIO_CONSUMER_H_
#include <linux/err.h>
struct device;
struct gpio_desc;
enum gpiod_flags { GPIOD_ASIS = 0, GPIOD_IN = 1, GPIOD_OUT_LOW = 3, GPIOD_OUT_HIGH = 7 };
static inline struct gpio_desc *devm_gpiod_get_optional(struct device *dev, const char *con, enum gpiod_flags f)
{ return NULL; }
static inline int gpiod_get_value_cansleep(const struct gpio_desc *d) { return 0; }
static inline void gpiod_set_value_cansleep(struct gpio_desc *d, int v) { }
static inline int gpiod_get_value(const struct gpio_desc *d) { return 0; }
static inline int gpiod_cansleep(const struct gpio_desc *d) { return 1; }
static inline int gpiod_to_irq(const struct gpio_desc *d) { return -ENXIO; }
#endif
