/* Stub <sys/mman.h> for the Switch (Horizon/newlib has no virtual-memory mmap).
 * Provides just enough declarations for Mesa to COMPILE; the few code paths that use
 * mmap (disk_cache file mapping, os_mman) are not exercised by NVK on the Switch — they
 * fall back to read()/malloc, or are guarded out. compat.c gives failing stubs so any
 * stray call returns MAP_FAILED rather than linking unresolved. */
#ifndef SWITCH_COMPAT_SYS_MMAN_H
#define SWITCH_COMPAT_SYS_MMAN_H

#include <sys/types.h>
#include <stddef.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *) -1)

#define MS_ASYNC      1
#define MS_SYNC       4
#define MS_INVALIDATE 2

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t len, int prot);
int   msync(void *addr, size_t length, int flags);
int   madvise(void *addr, size_t length, int advice);

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_COMPAT_SYS_MMAN_H */
