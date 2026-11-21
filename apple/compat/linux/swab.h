#include <asm/byteorder.h>
#include <sys/endian.h>
#ifndef swab32
#define swab16(x) bswap16(x)
#define swab32(x) bswap32(x)
#define swab64(x) bswap64(x)
#endif
