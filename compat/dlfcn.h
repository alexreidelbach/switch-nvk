/* Stub <dlfcn.h> for the Switch (Horizon has no dynamic loading; everything is statically linked).
 * Mesa's u_dl.c compiles its POSIX_LITE dlopen path against this; the stubs return "not found", so
 * util_dl_open() yields NULL and callers fall back (NVK is a static driver, it never dlopen()s). */
#ifndef SWITCH_COMPAT_DLFCN_H
#define SWITCH_COMPAT_DLFCN_H

#define RTLD_LAZY   0x001
#define RTLD_NOW    0x002
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL  0x000

#ifdef __cplusplus
extern "C" {
#endif

static inline void *dlopen(const char *file, int mode) { (void)file; (void)mode; return (void *)0; }
static inline int   dlclose(void *handle)              { (void)handle; return 0; }
static inline void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return (void *)0; }
static inline char *dlerror(void) { return (char *)"dynamic loading unsupported on Horizon"; }

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_COMPAT_DLFCN_H */
