# Apple M5 GPU DRM driver (the drm/asahi UAPI) over the kernel's apple_agx.
.include "../kconfig.mk"

SRCDIR=	${.CURDIR}

.PATH:	${SRCDIR}

KMOD=	agxdrm

SRCS=	agx_drv.c

SRCS	+=			\
	${LINUXKPI_GENSRCS}	\
	opt_acpi.h		\
	opt_drm.h		\
	opt_platform.h		\
	opt_splash.h		\
	opt_syscons.h		\
	opt_teken.h		\
	bus_if.h		\
	fb_if.h			\
	device_if.h		\
	vnode_if.h

CLEANFILES+= ${KMOD}.ko.full ${KMOD}.ko.debug

.include <bsd.kmod.mk>

.include "../linuxkpi_version.mk"
.include "../compiler_flags.mk"

CFLAGS+= -I${SRCDIR}/uapi -I${.CURDIR:H}/include/uapi/drm
CFLAGS+= -I${.CURDIR:H}/linuxkpi/gplv2/include
CFLAGS+= -I${.CURDIR:H}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include
CFLAGS+= -I${.CURDIR:H}/include
CFLAGS+= -I${.CURDIR:H}/include/drm
CFLAGS+= -I${.CURDIR:H}/include/uapi
CFLAGS+= -I${.CURDIR:H}/drivers/gpu

CFLAGS+= '-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+= '-DLINUXKPI_PARAM_PREFIX=agxdrm_'

CWARNFLAGS += -Wno-cast-qual
CWARNFLAGS += -Wno-gnu-pointer-arith
