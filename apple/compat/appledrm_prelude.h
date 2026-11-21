/* Force-included before every drm/apple source: what Linux headers pull in implicitly. */
#ifndef _APPLEDRM_PRELUDE_H_
#define _APPLEDRM_PRELUDE_H_
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
struct pci_dev;
#include "appledrm_compat.h"
#ifndef struct_group_attr
#define struct_group_attr(NAME, ATTRS, MEMBERS...) __struct_group(/* no tag */, NAME, ATTRS, MEMBERS)
#endif
#endif
