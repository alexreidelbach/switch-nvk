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

/* No demand-paged virtual memory on Horizon. NVK doesn't exercise these paths (disk_cache
 * mmap, os_mman); provide failing stubs so a stray call is well-defined rather than a link
 * error. If a real path ever needs them, back them with a malloc+pread shim. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
   (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
   errno = ENOMEM;
   return MAP_FAILED;
}

int munmap(void *addr, size_t length)      { (void)addr; (void)length; return 0; }
int mprotect(void *addr, size_t l, int p)  { (void)addr; (void)l; (void)p; return 0; }
int msync(void *addr, size_t l, int f)     { (void)addr; (void)l; (void)f; return 0; }
int madvise(void *addr, size_t l, int a)   { (void)addr; (void)l; (void)a; return 0; }

#endif /* __SWITCH__ */
