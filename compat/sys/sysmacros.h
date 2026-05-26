/* Stub <sys/sysmacros.h> for the Switch (newlib has no device-number macros).
 * Mesa includes it for major()/minor()/makedev() in DRM device handling; the Switch has
 * no /dev device nodes, so these values are inert. */
#ifndef SWITCH_COMPAT_SYS_SYSMACROS_H
#define SWITCH_COMPAT_SYS_SYSMACROS_H

#define major(dev)        ((int)(((unsigned)(dev) >> 8) & 0xff))
#define minor(dev)        ((int)((unsigned)(dev) & 0xff))
#define makedev(maj, min) ((((unsigned)(maj) & 0xff) << 8) | ((unsigned)(min) & 0xff))

#endif /* SWITCH_COMPAT_SYS_SYSMACROS_H */
