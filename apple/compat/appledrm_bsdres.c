/*
 * FreeBSD rman helpers for appledrm_platform.c, which cannot include sys/rman.h (its
 * struct resource clashes with Linux's). Resources are passed as opaque pointers.
 */
#define	resource	bsd_resource
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/rman.h>
#undef resource

/* Log an IRQ resource's state; activate it if the allocation left it inactive. */
int
appledrm_irq_res_check(device_t dev, void *res, int rid, const char *who)
{
	struct bsd_resource *r = res;
	int error = 0;

	printf("appledrm: %s: irq rid %d start %ju flags %#x%s\n", who, rid,
	    (uintmax_t)rman_get_start(r), rman_get_flags(r),
	    (rman_get_flags(r) & RF_ACTIVE) ? " active" : " NOT active");
	if ((rman_get_flags(r) & RF_ACTIVE) == 0) {
		error = bus_activate_resource(dev, SYS_RES_IRQ, rid, (struct resource *)r);
		printf("appledrm: %s: bus_activate_resource: %d\n", who, error);
	}
	return (error);
}
