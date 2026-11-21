#ifndef _APPLEDRM_LINUX_USB_TYPEC_DP_H_
#define _APPLEDRM_LINUX_USB_TYPEC_DP_H_
#include <linux/bits.h>
#include <linux/usb/typec_altmode.h>
struct typec_displayport_data {
	u32	status;
	u32	conf;
};
#define DP_STATUS_HPD_STATE	BIT(7)
#define DP_STATUS_IRQ_HPD	BIT(8)
#endif
