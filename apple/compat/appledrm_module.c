/*
 * Module identity and dependencies. drm/apple's own module_init()s (apple_drv.c,
 * mailbox.c, apple-dart.c) run as LinuxKPI SYSINITs when the module loads; probes that
 * find a provider not yet registered defer and are retried (appledrm_platform.c).
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>

static int
appledrm_modevent(module_t mod, int type, void *data)
{
	return (0);
}

static moduledata_t appledrm_mod = { "appledrm", appledrm_modevent, NULL };
DECLARE_MODULE(appledrm, appledrm_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(appledrm, 1);
MODULE_DEPEND(appledrm, drmn, 2, 2, 2);
MODULE_DEPEND(appledrm, dmabuf, 1, 1, 1);
MODULE_DEPEND(appledrm, linuxkpi, 1, 1, 1);
MODULE_DEPEND(appledrm, linuxkpi_video, 1, 1, 1);
