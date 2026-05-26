/* Switch (Horizon/newlib) compatibility shim for cross-building Mesa NVK.
 * Force-included into every Switch-target C/C++ TU via the cross-file c_args
 * (`-include /work/compat/switch_compat.h`). Centralizes the small libc/glibc-ism
 * gaps so the Mesa source stays pristine. Paired with /work/compat/compat.c (impls)
 * and the stub headers under /work/compat/sys/. */
#ifndef SWITCH_COMPAT_H
#define SWITCH_COMPAT_H

#ifdef __SWITCH__

/* newlib (with _GNU_SOURCE) DECLARES secure_getenv but never implements it; Mesa otherwise
 * emits its own `static inline secure_getenv`, which clashes with that extern declaration.
 * Tell Mesa the symbol exists (we implement it in compat.c → just getenv; the Switch has no
 * setuid/AT_SECURE, so this is the correct semantics). */
#define HAVE_SECURE_GETENV 1

/* alloca() is a builtin but newlib only prototypes it in <alloca.h>, which Mesa doesn't
 * include at every use site. Pull it in up front. */
#include <alloca.h>

/* Several TUs use NULL/size_t without an explicit <stddef.h>. */
#include <stddef.h>

#endif /* __SWITCH__ */
#endif /* SWITCH_COMPAT_H */
