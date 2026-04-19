/*
 * Copyright 2026 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifdef _WIN32

#include "vkr_win32_shim.h"

#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include "util/os_file.h"

#include "vkr_device.h"
#include "vkr_physical_device.h"

/*
 * Device map: lookup vkr_device from raw VkDevice handle.
 *
 * The shim functions receive raw VkDevice handles (after vn_replace) and
 * need access to the vkr_device to find Win32 function pointers and the
 * real (original) proc table entries.  We maintain a small table of
 * active devices for this purpose.
 */
#define VKR_WIN32_MAX_DEVICES 8

struct vkr_win32_device_entry {
   VkDevice handle;
   struct vkr_device *dev;
   /* Full copy of the proc table before any shim replacements.
    * Used to restore the original entries and for shim functions
    * that need to call through to the real driver.
    */
   struct vn_device_proc_table saved_proc_table;
};

static struct vkr_win32_device_entry vkr_win32_devices[VKR_WIN32_MAX_DEVICES];
static int vkr_win32_device_count;

static struct vkr_win32_device_entry *
vkr_win32_lookup_device(VkDevice device)
{
   for (int i = 0; i < vkr_win32_device_count; i++) {
      if (vkr_win32_devices[i].handle == device)
         return &vkr_win32_devices[i];
   }
   return NULL;
}

/*
 * Determine the host external memory handle type for a device.
 */
static VkExternalMemoryHandleTypeFlags
vkr_win32_get_host_handle_type(struct vkr_device *dev)
{
   if (dev->physical_device->host_external_memory_win32)
      return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
   return 0;
}

/* ------------------------------------------------------------------ */
/* pNext chain helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * Walk a pNext chain and remap OPAQUE_FD handle types to the host
 * equivalent (OPAQUE_WIN32).  This is needed because the guest Venus
 * driver speaks in fd terms but the Windows host driver uses Win32.
 */
static void
vkr_win32_remap_external_handle_types_in_chain(const void *pNext)
{
   for (VkBaseOutStructure *iter = (VkBaseOutStructure *)pNext; iter;
        iter = iter->pNext) {
      switch (iter->sType) {
      case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO: {
         VkExternalMemoryBufferCreateInfo *info =
            (VkExternalMemoryBufferCreateInfo *)iter;
         if (info->handleTypes & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) {
            info->handleTypes &= ~(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
            info->handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
         }
         break;
      }
      case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO: {
         VkExternalMemoryImageCreateInfo *info =
            (VkExternalMemoryImageCreateInfo *)iter;
         if (info->handleTypes & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) {
            info->handleTypes &= ~(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
            info->handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
         }
         break;
      }
      case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO: {
         VkExportMemoryAllocateInfo *info = (VkExportMemoryAllocateInfo *)iter;
         if (info->handleTypes & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) {
            info->handleTypes &= ~(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
            info->handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
         }
         break;
      }
      case VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO: {
         VkExportFenceCreateInfo *info = (VkExportFenceCreateInfo *)iter;
         if (info->handleTypes & VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT) {
            info->handleTypes &= ~VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
            info->handleTypes |= VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
         }
         break;
      }
      case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO: {
         VkExportSemaphoreCreateInfo *info = (VkExportSemaphoreCreateInfo *)iter;
         if (info->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
            info->handleTypes &= ~VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            info->handleTypes |= VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
         }
         break;
      }
      default:
         break;
      }
   }
}

/* ------------------------------------------------------------------ */
/* CreateDescriptorSetLayout shim                                      */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
vkr_win32_CreateDescriptorSetLayout(VkDevice device,
                                    const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    VkDescriptorSetLayout *pSetLayout)
{
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);
   if (!entry)
      return VK_ERROR_DEVICE_LOST;

   /* Intel's Windows Vulkan driver crashes in CreatePipelineLayout when
    * a push descriptor DSL has VkDescriptorSetLayoutBindingFlagsCreateInfo
    * in its pNext chain.  The driver accepts the DSL creation but later
    * segfaults when the DSL handle is referenced in a pipeline layout.
    * Strip the binding flags struct from push descriptor layouts.
    */
   if (pCreateInfo->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
      VkBaseOutStructure *prev = (VkBaseOutStructure *)pCreateInfo;
      while (prev->pNext) {
         if (prev->pNext->sType ==
             VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            prev->pNext = prev->pNext->pNext;
            break;
         }
         prev = prev->pNext;
      }
   }

   return entry->saved_proc_table.CreateDescriptorSetLayout(
      device, pCreateInfo, pAllocator, pSetLayout);
}

/* ------------------------------------------------------------------ */
/* CreateBuffer shim                                                   */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
vkr_win32_CreateBuffer(VkDevice device,
                       const VkBufferCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkBuffer *pBuffer)
{
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);
   if (!entry)
      return VK_ERROR_DEVICE_LOST;

   const VkExternalMemoryHandleTypeFlags host_type =
      vkr_win32_get_host_handle_type(entry->dev);

   /* Remap any existing external memory info in the pNext chain */
   vkr_win32_remap_external_handle_types_in_chain(pCreateInfo->pNext);

   /* If the device uses external memory and the guest didn't chain
    * VkExternalMemoryBufferCreateInfo, add one.  This is needed because
    * vkAllocateMemory forces HOST_VISIBLE memory to be external, but the
    * guest doesn't know this and creates buffers without the matching info.
    */
   VkBufferCreateInfo local_info;
   VkExternalMemoryBufferCreateInfo ext_info;
   const VkBufferCreateInfo *create_info = pCreateInfo;

   if (host_type) {
      bool has_external = false;
      for (const VkBaseInStructure *iter = (const VkBaseInStructure *)pCreateInfo->pNext;
           iter; iter = iter->pNext) {
         if (iter->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO) {
            has_external = true;
            break;
         }
      }

      if (!has_external) {
         local_info = *pCreateInfo;
         ext_info = (VkExternalMemoryBufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .pNext = (void *)local_info.pNext,
            .handleTypes = host_type,
         };
         local_info.pNext = &ext_info;
         create_info = &local_info;
      }
   }

   return entry->saved_proc_table.CreateBuffer(device, create_info, pAllocator, pBuffer);
}

/* ------------------------------------------------------------------ */
/* CreateImage shim                                                    */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
vkr_win32_CreateImage(VkDevice device,
                      const VkImageCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkImage *pImage)
{
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);
   if (!entry)
      return VK_ERROR_DEVICE_LOST;

   const VkExternalMemoryHandleTypeFlags host_type =
      vkr_win32_get_host_handle_type(entry->dev);

   /* Remap any existing external memory info in the pNext chain */
   vkr_win32_remap_external_handle_types_in_chain(pCreateInfo->pNext);

   /*
    * For any image that requests external memory (OPAQUE_WIN32 after remap),
    * force VK_IMAGE_TILING_LINEAR. Intel's Windows Vulkan driver picks a D3D-
    * oriented layout for OPAQUE_WIN32-exportable memory with TILING_OPTIMAL,
    * and that layout's byte order does NOT match the declared VkFormat. For
    * example, VK_FORMAT_B8G8R8A8_UNORM + OPTIMAL + OPAQUE_WIN32 ends up with
    * bytes laid out as if the format were DXGI-R8G8B8A8, so every later
    * consumer that takes the dma-buf at face value (kwin, XWayland, ANGLE)
    * sees R and B swapped.
    *
    * Forcing LINEAR tells Intel's driver to use a plain row-major layout
    * that respects the declared VkFormat byte order, which matches what
    * Vulkan, GL, libva and every Wayland compositor expect from a
    * dma-buf with modifier=LINEAR. Side effects: LINEAR is slower than
    * OPTIMAL for GPU sampling, but swapchain images are write-rarely-read-
    * once so the overhead is ~0 on GPUs that natively support both layouts
    * (all modern Intel/NVIDIA/AMD).
    *
    * The narrower version of this coercion (only when a
    * VkImageDrmFormatModifierListCreateInfoEXT is chained) lives in
    * vkr_dispatch_vkCreateImage; that path strips the modifier pNext and
    * then this broader coercion picks up every other external-memory image
    * Mesa might create for xcb/xlib DRI3, direct presentation, etc.
    */
   const VkImageCreateInfo *info_in = pCreateInfo;
   VkImageCreateInfo info_local;
   const void *pnext = info_in->pNext;
   const VkExternalMemoryImageCreateInfo *ext_info = NULL;
   for (const VkBaseInStructure *iter = (const VkBaseInStructure *)pnext;
        iter; iter = iter->pNext) {
      if (iter->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
         ext_info = (const VkExternalMemoryImageCreateInfo *)iter;
         break;
      }
   }
   if (ext_info && ext_info->handleTypes &&
       info_in->tiling != VK_IMAGE_TILING_LINEAR) {
      info_local = *info_in;
      info_local.tiling = VK_IMAGE_TILING_LINEAR;
      info_in = &info_local;
   }

   return entry->saved_proc_table.CreateImage(device, info_in, pAllocator, pImage);
}

/* ------------------------------------------------------------------ */
/* GetMemoryFdKHR shim                                                 */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
vkr_win32_GetMemoryFdKHR(VkDevice device,
                         const VkMemoryGetFdInfoKHR *pGetFdInfo,
                         int *pFd)
{
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);
   if (!entry || !entry->dev->GetMemoryWin32HandleKHR)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   VkExternalMemoryHandleTypeFlagBits win32_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
   if (pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      win32_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

   const VkMemoryGetWin32HandleInfoKHR win32_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
      .memory = pGetFdInfo->memory,
      .handleType = win32_type,
   };

   HANDLE handle = INVALID_HANDLE_VALUE;
   VkResult result =
      entry->dev->GetMemoryWin32HandleKHR(device, &win32_info, &handle);
   if (result != VK_SUCCESS)
      return result;

   int fd = os_wrap_win32_handle(handle);
   if (fd < 0) {
      CloseHandle(handle);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *pFd = fd;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* GetFenceFdKHR shim                                                  */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
vkr_win32_GetFenceFdKHR(VkDevice device,
                        const VkFenceGetFdInfoKHR *pGetFdInfo,
                        int *pFd)
{
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);
   if (!entry || !entry->dev->GetFenceWin32HandleKHR)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   const VkFenceGetWin32HandleInfoKHR win32_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_GET_WIN32_HANDLE_INFO_KHR,
      .fence = pGetFdInfo->fence,
      .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
   };

   HANDLE handle = INVALID_HANDLE_VALUE;
   VkResult result =
      entry->dev->GetFenceWin32HandleKHR(device, &win32_info, &handle);
   if (result != VK_SUCCESS)
      return result;

   int fd = os_wrap_win32_handle(handle);
   if (fd < 0) {
      CloseHandle(handle);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *pFd = fd;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* GetSemaphoreFdKHR shim                                              */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
vkr_win32_GetSemaphoreFdKHR(VkDevice device,
                            const VkSemaphoreGetFdInfoKHR *pGetFdInfo,
                            int *pFd)
{
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);
   if (!entry || !entry->dev->GetSemaphoreWin32HandleKHR)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   const VkSemaphoreGetWin32HandleInfoKHR win32_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
      .semaphore = pGetFdInfo->semaphore,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
   };

   HANDLE handle = INVALID_HANDLE_VALUE;
   VkResult result =
      entry->dev->GetSemaphoreWin32HandleKHR(device, &win32_info, &handle);
   if (result != VK_SUCCESS)
      return result;

   int fd = os_wrap_win32_handle(handle);
   if (fd < 0) {
      CloseHandle(handle);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *pFd = fd;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* ImportFenceFdKHR shim                                               */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
vkr_win32_ImportFenceFdKHR(VkDevice device,
                           const VkImportFenceFdInfoKHR *pImportFenceFdInfo)
{
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);
   if (!entry || !entry->dev->ImportFenceWin32HandleKHR)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   HANDLE import_handle = INVALID_HANDLE_VALUE;
   if (pImportFenceFdInfo->fd >= 0) {
      import_handle = os_get_win32_handle_from_fd(pImportFenceFdInfo->fd);
      if (import_handle == INVALID_HANDLE_VALUE)
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   HANDLE dup_handle = INVALID_HANDLE_VALUE;
   if (import_handle != INVALID_HANDLE_VALUE) {
      if (!DuplicateHandle(GetCurrentProcess(), import_handle,
                           GetCurrentProcess(), &dup_handle, 0, FALSE,
                           DUPLICATE_SAME_ACCESS))
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   const VkImportFenceWin32HandleInfoKHR win32_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_WIN32_HANDLE_INFO_KHR,
      .fence = pImportFenceFdInfo->fence,
      .flags = pImportFenceFdInfo->flags,
      .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
      .handle = dup_handle,
   };

   VkResult result =
      entry->dev->ImportFenceWin32HandleKHR(device, &win32_info);
   if (dup_handle != INVALID_HANDLE_VALUE && result != VK_SUCCESS)
      CloseHandle(dup_handle);
   return result;
}

/* ------------------------------------------------------------------ */
/* ImportSemaphoreFdKHR shim                                           */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
vkr_win32_ImportSemaphoreFdKHR(
   VkDevice device,
   const VkImportSemaphoreFdInfoKHR *pImportSemaphoreFdInfo)
{
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);
   if (!entry || !entry->dev->ImportSemaphoreWin32HandleKHR)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   HANDLE import_handle = INVALID_HANDLE_VALUE;
   if (pImportSemaphoreFdInfo->fd >= 0) {
      import_handle = os_get_win32_handle_from_fd(pImportSemaphoreFdInfo->fd);
      if (import_handle == INVALID_HANDLE_VALUE)
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   HANDLE dup_handle = INVALID_HANDLE_VALUE;
   if (import_handle != INVALID_HANDLE_VALUE) {
      if (!DuplicateHandle(GetCurrentProcess(), import_handle,
                           GetCurrentProcess(), &dup_handle, 0, FALSE,
                           DUPLICATE_SAME_ACCESS))
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   const VkImportSemaphoreWin32HandleInfoKHR win32_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
      .semaphore = pImportSemaphoreFdInfo->semaphore,
      .flags = pImportSemaphoreFdInfo->flags,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
      .handle = dup_handle,
   };

   VkResult result =
      entry->dev->ImportSemaphoreWin32HandleKHR(device, &win32_info);
   if (dup_handle != INVALID_HANDLE_VALUE && result != VK_SUCCESS)
      CloseHandle(dup_handle);
   return result;
}

/* ------------------------------------------------------------------ */
/* Deferred object destruction                                         */
/*                                                                     */
/* Intel's Windows Vulkan driver has a handle-caching bug: when a      */
/* non-dispatchable handle value (e.g. VkDescriptorSetLayout) is       */
/* recycled after the previous object was destroyed, the driver's      */
/* internal cache still holds stale metadata for that value.  This     */
/* causes crashes in vkCreatePipelineLayout and similar calls that     */
/* reference the "new" handle.                                         */
/*                                                                     */
/* The Vulkan validation layer masks this because it wraps every       */
/* handle in a unique dispatch object, so the driver never sees raw    */
/* value reuse.                                                        */
/*                                                                     */
/* Our fix: when the guest destroys an object, we skip the driver      */
/* destroy call and keep the driver handle alive.  This prevents the   */
/* driver from recycling the handle value.  All deferred handles are   */
/* actually destroyed at device teardown after DeviceWaitIdle.         */
/* ------------------------------------------------------------------ */

static void
vkr_win32_defer_destroy(struct vkr_device *dev, VkObjectType type, uint64_t handle)
{
   if (dev->deferred_destroy_count >= dev->deferred_destroy_capacity) {
      uint32_t new_cap = dev->deferred_destroy_capacity
                            ? dev->deferred_destroy_capacity * 2
                            : 64;
      struct vkr_win32_deferred_handle *new_arr =
         realloc(dev->deferred_destroys, new_cap * sizeof(*new_arr));
      if (!new_arr) {
         vkr_log("win32 shim: failed to grow deferred destroy list, leaking handle");
         return;
      }
      dev->deferred_destroys = new_arr;
      dev->deferred_destroy_capacity = new_cap;
   }
   dev->deferred_destroys[dev->deferred_destroy_count++] =
      (struct vkr_win32_deferred_handle){ .type = type, .handle = handle };
}

/*
 * Macro to define a shim that defers destruction instead of calling
 * the real driver vkDestroy*.  All Vulkan destroy functions for
 * non-dispatchable handles share the same signature:
 *   void vkDestroyFoo(VkDevice, VkFoo, const VkAllocationCallbacks*)
 */
#define VKR_WIN32_DEFERRED_DESTROY_SHIM(VkType, name, obj_type)                    \
static VKAPI_ATTR void VKAPI_CALL                                                  \
vkr_win32_Destroy##name(VkDevice device, VkType object,                             \
                        UNUSED const VkAllocationCallbacks *pAllocator)              \
{                                                                                    \
   if (!(uint64_t)object)                                                            \
      return;                                                                        \
   struct vkr_win32_device_entry *entry = vkr_win32_lookup_device(device);           \
   if (entry)                                                                        \
      vkr_win32_defer_destroy(entry->dev, obj_type, (uint64_t)object);               \
}

/* Generate shims for lightweight metadata object types.
 * Heavy resource types (Buffer, Image, DeviceMemory, Fence, Semaphore)
 * and pool types (CommandPool, DescriptorPool) are NOT deferred to
 * avoid exhausting GPU memory or breaking sub-resource management.
 */
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkDescriptorSetLayout, DescriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkPipelineLayout, PipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkPipeline, Pipeline, VK_OBJECT_TYPE_PIPELINE)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkRenderPass, RenderPass, VK_OBJECT_TYPE_RENDER_PASS)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkShaderModule, ShaderModule, VK_OBJECT_TYPE_SHADER_MODULE)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkFramebuffer, Framebuffer, VK_OBJECT_TYPE_FRAMEBUFFER)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkSampler, Sampler, VK_OBJECT_TYPE_SAMPLER)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkImageView, ImageView, VK_OBJECT_TYPE_IMAGE_VIEW)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkBufferView, BufferView, VK_OBJECT_TYPE_BUFFER_VIEW)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkDescriptorUpdateTemplate, DescriptorUpdateTemplate, VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkSamplerYcbcrConversion, SamplerYcbcrConversion, VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkPipelineCache, PipelineCache, VK_OBJECT_TYPE_PIPELINE_CACHE)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkEvent, Event, VK_OBJECT_TYPE_EVENT)
VKR_WIN32_DEFERRED_DESTROY_SHIM(VkQueryPool, QueryPool, VK_OBJECT_TYPE_QUERY_POOL)

/* ------------------------------------------------------------------ */
/* Install / remove                                                    */
/* ------------------------------------------------------------------ */

void
vkr_win32_device_install_shim(struct vkr_device *dev)
{
   if (vkr_win32_device_count >= VKR_WIN32_MAX_DEVICES) {
      vkr_log("win32 shim: too many devices, shim not installed");
      return;
   }

   struct vn_device_proc_table *vk = &dev->proc_table;
   struct vkr_win32_device_entry *entry =
      &vkr_win32_devices[vkr_win32_device_count++];

   entry->handle = dev->base.handle.device;
   entry->dev = dev;

   /* Save the entire proc table so we can restore it on removal */
   entry->saved_proc_table = *vk;

   /* --- External memory/sync fd→Win32 translation shims --- */
   if (dev->physical_device->host_external_memory_win32 ||
       dev->physical_device->host_external_fence_win32 ||
       dev->physical_device->host_external_semaphore_win32) {
      vk->CreateBuffer = vkr_win32_CreateBuffer;
      vk->CreateImage = vkr_win32_CreateImage;

      if (dev->GetMemoryWin32HandleKHR)
         vk->GetMemoryFdKHR = vkr_win32_GetMemoryFdKHR;
      if (dev->GetFenceWin32HandleKHR)
         vk->GetFenceFdKHR = vkr_win32_GetFenceFdKHR;
      if (dev->GetSemaphoreWin32HandleKHR)
         vk->GetSemaphoreFdKHR = vkr_win32_GetSemaphoreFdKHR;
      if (dev->ImportFenceWin32HandleKHR)
         vk->ImportFenceFdKHR = vkr_win32_ImportFenceFdKHR;
      if (dev->ImportSemaphoreWin32HandleKHR)
         vk->ImportSemaphoreFdKHR = vkr_win32_ImportSemaphoreFdKHR;
   }

   /* --- Intel driver workaround shims --- */
   vk->CreateDescriptorSetLayout = vkr_win32_CreateDescriptorSetLayout;

   /* --- Deferred destruction shims ---
    *
    * Replace all Destroy/Free functions with shims that record the
    * handle for later destruction instead of calling the driver
    * immediately.  This prevents the Intel driver from recycling
    * handle values and hitting its stale-cache bug.
    */
   /* Defer destruction of lightweight metadata objects whose handles
    * are commonly recycled by the Intel driver.  Heavy resource types
    * (Buffer, Image, DeviceMemory) are NOT deferred — keeping them
    * alive would exhaust GPU memory.  Similarly, pool types (CommandPool,
    * DescriptorPool) manage sub-resources and are left alone.
    */
   vk->DestroyDescriptorSetLayout = vkr_win32_DestroyDescriptorSetLayout;
   vk->DestroyPipelineLayout = vkr_win32_DestroyPipelineLayout;
   vk->DestroyPipeline = vkr_win32_DestroyPipeline;
   vk->DestroyRenderPass = vkr_win32_DestroyRenderPass;
   vk->DestroyShaderModule = vkr_win32_DestroyShaderModule;
   vk->DestroyFramebuffer = vkr_win32_DestroyFramebuffer;
   vk->DestroySampler = vkr_win32_DestroySampler;
   vk->DestroyImageView = vkr_win32_DestroyImageView;
   vk->DestroyBufferView = vkr_win32_DestroyBufferView;
   vk->DestroyDescriptorUpdateTemplate = vkr_win32_DestroyDescriptorUpdateTemplate;
   vk->DestroySamplerYcbcrConversion = vkr_win32_DestroySamplerYcbcrConversion;
   vk->DestroyPipelineCache = vkr_win32_DestroyPipelineCache;
   vk->DestroyEvent = vkr_win32_DestroyEvent;
   vk->DestroyQueryPool = vkr_win32_DestroyQueryPool;

}

void
vkr_win32_device_remove_shim(struct vkr_device *dev)
{
   for (int i = 0; i < vkr_win32_device_count; i++) {
      if (vkr_win32_devices[i].dev == dev) {
         struct vkr_win32_device_entry *entry = &vkr_win32_devices[i];

         /* Restore the entire proc table from the saved copy.
          * This undoes all shim replacements (create, destroy, export, import).
          */
         dev->proc_table = entry->saved_proc_table;

         /* Remove from table by swapping with last */
         vkr_win32_devices[i] = vkr_win32_devices[--vkr_win32_device_count];
         return;
      }
   }
}

void
vkr_win32_flush_deferred_destroys(struct vkr_device *dev)
{
   if (!dev->deferred_destroy_count) {
      free(dev->deferred_destroys);
      dev->deferred_destroys = NULL;
      dev->deferred_destroy_capacity = 0;
      return;
   }

   /* At this point the proc table has been restored to real driver
    * functions (by vkr_win32_device_remove_shim) and DeviceWaitIdle
    * has completed, so it is safe to destroy these objects.
    */
   struct vn_device_proc_table *vk = &dev->proc_table;
   VkDevice device = dev->base.handle.device;

   for (uint32_t i = 0; i < dev->deferred_destroy_count; i++) {
      struct vkr_win32_deferred_handle *h = &dev->deferred_destroys[i];
      switch (h->type) {
      case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
         vk->DestroyDescriptorSetLayout(device, (VkDescriptorSetLayout)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
         vk->DestroyPipelineLayout(device, (VkPipelineLayout)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_PIPELINE:
         vk->DestroyPipeline(device, (VkPipeline)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_RENDER_PASS:
         vk->DestroyRenderPass(device, (VkRenderPass)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_SHADER_MODULE:
         vk->DestroyShaderModule(device, (VkShaderModule)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_FRAMEBUFFER:
         vk->DestroyFramebuffer(device, (VkFramebuffer)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_SAMPLER:
         vk->DestroySampler(device, (VkSampler)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_IMAGE_VIEW:
         vk->DestroyImageView(device, (VkImageView)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_BUFFER_VIEW:
         vk->DestroyBufferView(device, (VkBufferView)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE:
         vk->DestroyDescriptorUpdateTemplate(device, (VkDescriptorUpdateTemplate)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION:
         vk->DestroySamplerYcbcrConversion(device, (VkSamplerYcbcrConversion)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_PIPELINE_CACHE:
         vk->DestroyPipelineCache(device, (VkPipelineCache)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_EVENT:
         vk->DestroyEvent(device, (VkEvent)h->handle, NULL);
         break;
      case VK_OBJECT_TYPE_QUERY_POOL:
         vk->DestroyQueryPool(device, (VkQueryPool)h->handle, NULL);
         break;
      default:
         vkr_log("win32 shim: unexpected deferred type %u", (uint32_t)h->type);
         break;
      }
   }

   free(dev->deferred_destroys);
   dev->deferred_destroys = NULL;
   dev->deferred_destroy_count = 0;
   dev->deferred_destroy_capacity = 0;
}

#endif /* _WIN32 */
