#ifndef _APPLEDRM_LINUX_INT_LOG_H_
#define _APPLEDRM_LINUX_INT_LOG_H_
#include <linux/types.h>
#include <linux/log2.h>
/* Linux lib/math/int_log.c: log2 in 8.24 fixed point. */
static inline unsigned int intlog2(u32 value)
{
	unsigned int msb, frac;
	if (value == 0)
		return 0;
	msb = fls(value) - 1;
	/* linear interpolation of the fraction; enough for metadata scaling */
	frac = (u32)(((u64)value << (31 - msb)) & 0x7fffffff) >> 7;
	return (msb << 24) + frac;
}
#endif
