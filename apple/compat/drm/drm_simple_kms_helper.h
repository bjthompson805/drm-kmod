/* The part of Linux drm_simple_kms_helper.h drm/apple uses (drm-kmod no longer ships it). */
#ifndef _APPLEDRM_DRM_SIMPLE_KMS_HELPER_H_
#define _APPLEDRM_DRM_SIMPLE_KMS_HELPER_H_
#include <drm/drm_encoder.h>
int drm_simple_encoder_init(struct drm_device *dev, struct drm_encoder *encoder, int encoder_type);
#define drmm_simple_encoder_alloc(dev, type, member, encoder_type) \
	((type *)drmm_encoder_alloc(dev, type, member, NULL, encoder_type, NULL))
#endif
