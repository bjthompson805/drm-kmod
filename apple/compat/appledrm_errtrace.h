/*
 * Bring-up aid, force-included into selected files (Makefile CFLAGS.<file>): each use of
 * these errno values logs its file:line once, so a silent "return -EINVAL" in a probe is
 * visible. Remove from the Makefile once the display probes cleanly.
 */
#ifndef _APPLEDRM_ERRTRACE_H_
#define _APPLEDRM_ERRTRACE_H_
#include <sys/errno.h>
int appledrm_errtrace(const char *file, int line, const char *name, int val);
#undef EINVAL
#undef ENODEV
#undef ENOENT
#undef EOPNOTSUPP
#define EINVAL		appledrm_errtrace(__FILE__, __LINE__, "EINVAL", 22)
#define ENODEV		appledrm_errtrace(__FILE__, __LINE__, "ENODEV", 19)
#define ENOENT		appledrm_errtrace(__FILE__, __LINE__, "ENOENT", 2)
#define EOPNOTSUPP	appledrm_errtrace(__FILE__, __LINE__, "EOPNOTSUPP", 45)
#endif
