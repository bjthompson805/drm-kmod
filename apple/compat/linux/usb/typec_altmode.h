#ifndef _APPLEDRM_LINUX_USB_TYPEC_ALTMODE_H_
#define _APPLEDRM_LINUX_USB_TYPEC_ALTMODE_H_
#include <linux/types.h>
#ifndef USB_TYPEC_DP_SID
#define USB_TYPEC_DP_SID	0xff01
#endif
#define TYPEC_STATE_SAFE	0
#define TYPEC_STATE_USB		1
#define TYPEC_STATE_MODAL	2
enum { TYPEC_DP_STATE_A = TYPEC_STATE_MODAL, TYPEC_DP_STATE_B, TYPEC_DP_STATE_C,
	TYPEC_DP_STATE_D, TYPEC_DP_STATE_E, TYPEC_DP_STATE_F };
struct typec_altmode {
	u16		svid;
	int		mode;
	u32		vdo;
	unsigned int	active:1;
};
#endif
