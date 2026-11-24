# Apple DCP display driver (aurora-silicon/linux drivers/gpu/drm/apple @ b4cd7b3) on LinuxKPI.
.include "../kconfig.mk"

SRCDIR=	${.CURDIR}/src

.PATH:	${SRCDIR} ${SRCDIR}/epic ${.CURDIR}/soc ${.CURDIR}/iommu ${.CURDIR}/compat ${.CURDIR}/drmhelpers

KMOD=	appledrm

# drm/apple Makefile's appledrm-y (CONFIG_DRM_APPLE_AUDIO and T8132 off), soc/apple pieces, iommu.
SRCS=	apple_drv.c apple_resume.c afk.c dcp.c dcp-fabric.c dcp-fabric-core.c dcp_backlight.c dptxep.c iomfb.c \
	parser.c systemep.c dcp-hdmi.c connector.c dcp-panel-adt.c dcp-display-metadata.c \
	dcp-180hz.c ibootep.c dcpext_scanout.c  iomfb_v12_3.c iomfb_v13_3.c \
	iomfb_v14_7_tpl.c iomfb_v26_6_desktop.c iomfb_v14_7.c iomfb_v14_7_link.c iomfb_h17p.c \
	iomfb_v26_6.c iomfb_v26_6_link.c  iomfb_v27.c dpavservep.c \
	panelavep.c plane.c dcp-indicator.c \
	rtkit.c rtkit-crashlog.c mailbox.c tunable.c io-pgtable-dart.c apple-dart.c \
	drm_gem_dma_helper.c drm_fb_dma_helper.c drm_fbdev_dma.c drm_gem_fb_extra.c \
	appledrm_of.c appledrm_platform.c appledrm_iommu.c appledrm_component.c appledrm_misc.c appledrm_module.c appledrm_bsdres.c

SRCS	+=			\
	${LINUXKPI_GENSRCS}	\
	opt_acpi.h		\
	opt_drm.h		\
	opt_platform.h		\
	opt_splash.h		\
	opt_syscons.h		\
	opt_teken.h		\
	backlight_if.h		\
	bus_if.h		\
	fb_if.h			\
	device_if.h		\
	ofw_bus_if.h		\
	vnode_if.h

CLEANFILES+= ${KMOD}.ko.full ${KMOD}.ko.debug

.include <bsd.kmod.mk>

.include "../linuxkpi_version.mk"
.include "../compiler_flags.mk"

CFLAGS+= -I${.CURDIR}/compat
CFLAGS+= -I${.CURDIR}/drmhelpers
CFLAGS+= -I${.CURDIR}/iommu
CFLAGS+= -I${.CURDIR}/include
CFLAGS+= -I${.CURDIR:H}/drivers/gpu
CFLAGS+= -include ${.CURDIR}/compat/appledrm_prelude.h
CFLAGS+= -I${.CURDIR:H}/linuxkpi/gplv2/include
CFLAGS+= -I${.CURDIR:H}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include
CFLAGS+= -I${.CURDIR:H}/include
CFLAGS+= -I${.CURDIR:H}/include/drm
CFLAGS+= -I${.CURDIR:H}/include/uapi
CFLAGS+= -I${SRCDIR}

CFLAGS+= '-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+= '-DLINUXKPI_PARAM_PREFIX=appledrm_'

CWARNFLAGS += -Wno-cast-qual
CFLAGS+= ${KCONFIG:C/(.*)/-DCONFIG_\1/}
CFLAGS+= -DCONFIG_OF -DCONFIG_DRM_APPLE_DCP_T8142 -DCONFIG_IOMMU_API -DCONFIG_APPLE_DART -DCONFIG_ARM64 -DCONFIG_DRM_APPLE
CWARNFLAGS += -Wno-format -Wno-gnu-pointer-arith -Wno-missing-prototypes -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
CFLAGS+= -DDRM_SYSCTL_PARAM_PREFIX=_dri
CWARNFLAGS += -Wno-macro-redefined -Wno-unused-label
CFLAGS+= -ferror-limit=0
CFLAGS.drm_gem_fb_extra.c+= -I${.CURDIR:H}/drivers/gpu/drm
