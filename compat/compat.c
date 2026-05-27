/* Switch (Horizon/newlib) compat implementations for symbols Mesa expects but newlib
 * declares-without-implementing (or lacks). Compiled into the Switch build and linked in.
 * See switch_compat.h + sys/mman.h. */
#ifdef __SWITCH__

#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include "sys/mman.h"

/* The Switch has no setuid/AT_SECURE notion; secure_getenv == getenv. */
char *secure_getenv(const char *name)
{
   return getenv(name);
}

/* mmap/munmap live in winsys/switch_libc_shim.c: BO mapping has to route through
 * the DRM shim (mmap(fd, map_handle) -> drm_shim_mmap), which this TU can't see.
 * mprotect/msync/madvise stay here (pure no-ops, no shim dependency). */
int mprotect(void *addr, size_t l, int p)  { (void)addr; (void)l; (void)p; return 0; }
int msync(void *addr, size_t l, int f)     { (void)addr; (void)l; (void)f; return 0; }
int madvise(void *addr, size_t l, int a)   { (void)addr; (void)l; (void)a; return 0; }

#endif /* __SWITCH__ */
