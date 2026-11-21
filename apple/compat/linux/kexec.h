/* kexec does not exist on FreeBSD; aurora's kexec blockers are bookkeeping only. */
#ifndef _APPLEDRM_LINUX_KEXEC_H_
#define _APPLEDRM_LINUX_KEXEC_H_
#include <linux/atomic.h>
struct kexec_blocker {
	atomic_t	users;
	const char	*reason;
};
static inline int kexec_block(struct kexec_blocker *b) { atomic_inc(&b->users); return 0; }
static inline void kexec_unblock(struct kexec_blocker *b) { atomic_dec(&b->users); }
#endif
