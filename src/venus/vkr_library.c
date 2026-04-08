/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_common.h"
#include "vkr_library.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

void
vkr_library_preload_icd(void)
{
#ifdef ENABLE_VULKAN_PRELOAD
   struct vulkan_library lib = { 0 };

   if (!vkr_library_load(&lib))
      return;

   /* Get vkGetInstanceProcAddr from libvulkan */
   PFN_vkGetInstanceProcAddr get_proc_addr = lib.GetInstanceProcAddr;

   PFN_vkEnumerateInstanceExtensionProperties enumerate_inst_ext_props =
      (PFN_vkEnumerateInstanceExtensionProperties)get_proc_addr(
         VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
   if (enumerate_inst_ext_props) {
      /* this makes the Vulkan loader loads ICDs */
      uint32_t unused_count;
      enumerate_inst_ext_props(NULL, &unused_count, NULL);
   }

   vkr_library_unload(&lib);
#endif
}

#if defined(ENABLE_VULKAN_DLOAD)

bool
vkr_library_load(struct vulkan_library *lib)
{
   if (lib->handle)
      return true;

#ifdef _WIN32
   lib->handle = LoadLibraryA("vulkan-1.dll");
   if (lib->handle == NULL) {
      vkr_log("failed to open vulkan-1.dll: error %lu", GetLastError());
      return false;
   }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   lib->GetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)GetProcAddress(lib->handle, "vkGetInstanceProcAddr");
#pragma GCC diagnostic pop

#else /* !_WIN32 */
   lib->handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (lib->handle == NULL)
      lib->handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
   if (lib->handle == NULL) {
      vkr_log("failed to open libvulkan: %s", dlerror());
      return false;
   }

   /* Clear any existing error */
   dlerror();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   lib->GetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)dlsym(lib->handle, "vkGetInstanceProcAddr");
#pragma GCC diagnostic pop

   char *error = dlerror();
   if (error != NULL) {
      vkr_log("dlerror: %s", error);
      goto fail;
   }
#endif /* _WIN32 */

   if (lib->GetInstanceProcAddr == NULL) {
      vkr_log("failed to load vkGetInstanceProcAddr");
      goto fail;
   }

   return true;

fail:
#ifdef _WIN32
   FreeLibrary(lib->handle);
#else
   dlclose(lib->handle);
#endif
   lib->handle = NULL;
   return false;
}

void
vkr_library_unload(struct vulkan_library *lib)
{
   if (lib->handle) {
#ifdef _WIN32
      FreeLibrary(lib->handle);
#else
      dlclose(lib->handle);
#endif
      lib->GetInstanceProcAddr = NULL;
      lib->handle = NULL;
   }
}

#endif /* ENABLE_VULKAN_DLOAD */
