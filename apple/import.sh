#!/bin/sh
# Re-import drm/apple + soc/apple + io-pgtable-dart from aurora (ref/aurora/linux-b4cd7b3) and apply the
# mechanical Linux 7.3 -> drm-kmod 6.12 renames. Local FreeBSD edits belong in compat/ or in
# patches/*.diff (applied after the renames), never directly in src/ or soc/.
set -e
R=$HOME/m1n1-env/ref/aurora/linux-b4cd7b3
D=$(cd $(dirname $0) && pwd)
rm -rf $D/src $D/soc $D/iommu $D/include/linux/soc/apple
mkdir -p $D/src $D/soc $D/iommu $D/include/linux/soc
cp -R $R/drivers/gpu/drm/apple/. $D/src/
cp $R/drivers/soc/apple/rtkit.c $R/drivers/soc/apple/rtkit-internal.h $R/drivers/soc/apple/rtkit-v27-late.inc \
   $R/drivers/soc/apple/rtkit-crashlog.c $R/drivers/soc/apple/mailbox.c $R/drivers/soc/apple/tunable.c $R/drivers/soc/apple/mailbox-g17-test.h $D/soc/
cp $R/drivers/iommu/io-pgtable-dart.c $R/drivers/iommu/apple-dart* $R/drivers/iommu/iommu-pages.h $R/drivers/iommu/dma-iommu.h $D/iommu/
cp $R/include/linux/io-pgtable.h $R/include/linux/iommu.h $D/include/linux/
cp -R $R/include/linux/soc/apple $D/include/linux/soc/
# Linux 7.x renamed struct drm_atomic_state to struct drm_atomic_commit (the drm_atomic_commit()
# function keeps its name).
find $D/src $D/soc $D/include -type f \( -name '*.[ch]' -o -name '*.inc' \) -exec sed -i '' -E \
	-e 's/struct drm_atomic_commit[[:>:]]/struct drm_atomic_state/g' \
	-e 's/drm_atomic_commit_(alloc|clear|get|put|init|default_clear|default_release)\(/drm_atomic_state_\1(/g' \
	{} +
# drm-kmod 6.12's connector .mode_valid takes a non-const mode (const since Linux 6.15); same ABI.
sed -i '' -E 's/^(	\.mode_valid[[:space:]]+= )dcp_mode_valid,/\1(enum drm_mode_status (*)(struct drm_connector *, struct drm_display_mode *))dcp_mode_valid,/' $D/src/apple_drv.c
# drm-kmod 6.12 drm_helper_mode_fill_fb_struct() takes (dev, fb, mode_cmd); 7.x adds the format info.
perl -0pi -e 's/drm_helper_mode_fill_fb_struct\(drm, fb,\s*drm_get_format_info\(drm, cmd\.pixel_format, cmd\.modifier\[0\]\),\s*&cmd\)/drm_helper_mode_fill_fb_struct(drm, fb, &cmd)/' $D/src/dcp-indicator.c
for p in $D/patches/*.diff; do [ -f "$p" ] && patch -s -d $D -p1 < "$p"; done
echo "imported"
