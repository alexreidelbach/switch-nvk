/*
 * main_linktest.c — minimal entry to prove the NVK driver + winsys shim + libc
 * shim link into a real Switch EXE (resolving the app/linker-script group via
 * devkitA64 switch.specs/crt0). libnvk.a is whole-archived at link, so the whole
 * driver must resolve; this main just needs to exist and reference the ICD entry.
 *
 * The actual vkCreateInstance -> enumerate GM20B -> triangle -> present is M3.
 */
#include <stdio.h>

/* The Vulkan ICD's guaranteed export. Referencing it keeps the driver entry
 * reachable; whole-archive pulls the rest. */
extern void *vk_icdGetInstanceProcAddr(void *instance, const char *pName);

int main(void)
{
   volatile void *icd = (void *)&vk_icdGetInstanceProcAddr;
   printf("switch-nvk link test: ICD entry @ %p\n", (void *)icd);
   return 0;
}
