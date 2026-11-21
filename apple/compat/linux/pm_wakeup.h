#ifndef _APPLEDRM_LINUX_PM_WAKEUP_H_
#define _APPLEDRM_LINUX_PM_WAKEUP_H_
#include <linux/device.h>
static inline bool device_may_wakeup(struct device *dev) { return false; }
static inline int device_init_wakeup(struct device *dev, bool enable) { return 0; }
#endif
