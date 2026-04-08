/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_device.h"

#include "venus-protocol/vn_protocol_renderer_device.h"

#include "util/os_file.h"

#include "vkr_command_buffer.h"
#include "vkr_context.h"
#include "vkr_descriptor_set.h"
#include "vkr_device_memory.h"
#include "vkr_physical_device.h"
#include "vkr_queue.h"
#include "vkr_win32_shim.h"

static bool
vkr_device_has_extension(const char *const *exts, uint32_t count, const char *name)
{
   for (uint32_t i = 0; i < count; i++) {
      if (!strcmp(exts[i], name))
         return true;
   }

   return false;
}

static const char *
vkr_device_map_guest_ext_to_host(const struct vkr_physical_device *physical_dev,
                                 const char *name)
{
#ifdef _WIN32
   if (!strcmp(name, "VK_KHR_external_memory_fd") &&
       physical_dev->host_external_memory_win32)
      return "VK_KHR_external_memory_win32";
   if (!strcmp(name, "VK_KHR_external_fence_fd") &&
       physical_dev->host_external_fence_win32)
      return "VK_KHR_external_fence_win32";
   if (!strcmp(name, "VK_KHR_external_semaphore_fd") &&
       physical_dev->host_external_semaphore_win32)
      return "VK_KHR_external_semaphore_win32";
#endif

   return name;
}

static VkResult
vkr_device_create_queues(struct vkr_context *ctx,
                         struct vkr_device *dev,
                         uint32_t create_info_count,
                         const VkDeviceQueueCreateInfo *create_infos)
{
   struct vn_device_proc_table *vk = &dev->proc_table;
   list_inithead(&dev->queues);

   for (uint32_t i = 0; i < create_info_count; i++) {
      for (uint32_t j = 0; j < create_infos[i].queueCount; j++) {
         const VkDeviceQueueInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
            .pNext = NULL,
            .flags = create_infos[i].flags,
            .queueFamilyIndex = create_infos[i].queueFamilyIndex,
            .queueIndex = j,
         };
         VkQueue handle = VK_NULL_HANDLE;
         /* There was a bug in spec which forbids usage of vkGetDeviceQueue2
          * with flags set to zero. It was fixed in spec version 1.1.130.
          * Work around drivers that are implementing this buggy behavior
          */
         if (info.flags) {
            vk->GetDeviceQueue2(dev->base.handle.device, &info, &handle);
         } else {
            vk->GetDeviceQueue(dev->base.handle.device, info.queueFamilyIndex,
                               info.queueIndex, &handle);
         }

         struct vkr_queue *queue = vkr_queue_create(
            ctx, dev, info.flags, info.queueFamilyIndex, info.queueIndex, handle);
         if (!queue) {
            list_for_each_entry_safe (struct vkr_queue, entry, &dev->queues, base.track_head)
               vkr_queue_destroy(ctx, entry);

            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }

         /* queues are not tracked as device objects */
         list_add(&queue->base.track_head, &dev->queues);
      }
   }

   return VK_SUCCESS;
}

static void
vkr_device_init_proc_table(struct vkr_device *dev,
                           uint32_t api_version,
                           const char *const *exts,
                           uint32_t count)
{
   assert(dev->physical_device);
   struct vn_physical_device_proc_table *vk = &dev->physical_device->proc_table;

   struct vn_info_extension_table ext_table;
   vkr_extension_table_init(&ext_table, exts, count);

   vn_util_init_device_proc_table(dev->base.handle.device, vk->GetDeviceProcAddr,
                                  api_version, &ext_table, &dev->proc_table);

#ifdef _WIN32
   if (dev->physical_device->host_external_memory_win32) {
      dev->GetMemoryWin32HandleKHR =
         (PFN_vkGetMemoryWin32HandleKHR)vk->GetDeviceProcAddr(dev->base.handle.device,
                                                              "vkGetMemoryWin32HandleKHR");
      dev->GetMemoryWin32HandlePropertiesKHR =
         (PFN_vkGetMemoryWin32HandlePropertiesKHR)vk->GetDeviceProcAddr(
            dev->base.handle.device, "vkGetMemoryWin32HandlePropertiesKHR");
   }

   if (dev->physical_device->host_external_fence_win32) {
      dev->GetFenceWin32HandleKHR =
         (PFN_vkGetFenceWin32HandleKHR)vk->GetDeviceProcAddr(dev->base.handle.device,
                                                             "vkGetFenceWin32HandleKHR");
      dev->ImportFenceWin32HandleKHR =
         (PFN_vkImportFenceWin32HandleKHR)vk->GetDeviceProcAddr(
            dev->base.handle.device, "vkImportFenceWin32HandleKHR");
   }

   if (dev->physical_device->host_external_semaphore_win32) {
      dev->GetSemaphoreWin32HandleKHR =
         (PFN_vkGetSemaphoreWin32HandleKHR)vk->GetDeviceProcAddr(
            dev->base.handle.device, "vkGetSemaphoreWin32HandleKHR");
      dev->ImportSemaphoreWin32HandleKHR =
         (PFN_vkImportSemaphoreWin32HandleKHR)vk->GetDeviceProcAddr(
            dev->base.handle.device, "vkImportSemaphoreWin32HandleKHR");
   }
#endif
}

VkResult
vkr_device_get_fence_fd(struct vkr_device *dev,
                        VkFence fence,
                        VkExternalFenceHandleTypeFlagBits handle_type,
                        int *out_fd)
{
   struct vn_device_proc_table *vk = &dev->proc_table;

#ifdef _WIN32
   if (handle_type == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT &&
       dev->physical_device->host_external_fence_win32 && dev->GetFenceWin32HandleKHR) {
      HANDLE handle = INVALID_HANDLE_VALUE;
      const VkFenceGetWin32HandleInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_FENCE_GET_WIN32_HANDLE_INFO_KHR,
         .fence = fence,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
      };
      VkResult result =
         dev->GetFenceWin32HandleKHR(dev->base.handle.device, &info, &handle);
      if (result != VK_SUCCESS)
         return result;

      const int fd = os_wrap_win32_handle(handle);
      if (fd < 0) {
         CloseHandle(handle);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      *out_fd = fd;
      return VK_SUCCESS;
   }
#endif

   const VkFenceGetFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
      .fence = fence,
      .handleType = handle_type,
   };
   return vk->GetFenceFdKHR(dev->base.handle.device, &info, out_fd);
}

VkResult
vkr_device_get_semaphore_fd(struct vkr_device *dev,
                            VkSemaphore semaphore,
                            VkExternalSemaphoreHandleTypeFlagBits handle_type,
                            int *out_fd)
{
   struct vn_device_proc_table *vk = &dev->proc_table;

#ifdef _WIN32
   if (handle_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT &&
       dev->physical_device->host_external_semaphore_win32 &&
       dev->GetSemaphoreWin32HandleKHR) {
      HANDLE handle = INVALID_HANDLE_VALUE;
      const VkSemaphoreGetWin32HandleInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
         .semaphore = semaphore,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
      };
      VkResult result =
         dev->GetSemaphoreWin32HandleKHR(dev->base.handle.device, &info, &handle);
      if (result != VK_SUCCESS)
         return result;

      const int fd = os_wrap_win32_handle(handle);
      if (fd < 0) {
         CloseHandle(handle);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      *out_fd = fd;
      return VK_SUCCESS;
   }
#endif

   const VkSemaphoreGetFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .semaphore = semaphore,
      .handleType = handle_type,
   };
   return vk->GetSemaphoreFdKHR(dev->base.handle.device, &info, out_fd);
}

VkResult
vkr_device_import_fence_fd(struct vkr_device *dev,
                           const VkImportFenceFdInfoKHR *import_info)
{
   struct vn_device_proc_table *vk = &dev->proc_table;

#ifdef _WIN32
   if (import_info->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT &&
       dev->physical_device->host_external_fence_win32 && dev->ImportFenceWin32HandleKHR) {
      HANDLE import_handle = INVALID_HANDLE_VALUE;
      HANDLE dup_handle = INVALID_HANDLE_VALUE;

      if (import_info->fd >= 0) {
         import_handle = os_get_win32_handle_from_fd(import_info->fd);
         if (import_handle == INVALID_HANDLE_VALUE)
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

         if (!DuplicateHandle(GetCurrentProcess(), import_handle, GetCurrentProcess(),
                              &dup_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }

      const VkImportFenceWin32HandleInfoKHR win32_info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_WIN32_HANDLE_INFO_KHR,
         .fence = import_info->fence,
         .flags = import_info->flags,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
         .handle = dup_handle,
         .name = NULL,
      };
      const VkResult result =
         dev->ImportFenceWin32HandleKHR(dev->base.handle.device, &win32_info);
      if (dup_handle != INVALID_HANDLE_VALUE)
         CloseHandle(dup_handle);
      return result;
   }
#endif

   return vk->ImportFenceFdKHR(dev->base.handle.device, import_info);
}

VkResult
vkr_device_import_semaphore_fd(struct vkr_device *dev,
                               const VkImportSemaphoreFdInfoKHR *import_info)
{
   struct vn_device_proc_table *vk = &dev->proc_table;

#ifdef _WIN32
   if (import_info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT &&
       dev->physical_device->host_external_semaphore_win32 &&
       dev->ImportSemaphoreWin32HandleKHR) {
      HANDLE import_handle = INVALID_HANDLE_VALUE;
      HANDLE dup_handle = INVALID_HANDLE_VALUE;

      if (import_info->fd >= 0) {
         import_handle = os_get_win32_handle_from_fd(import_info->fd);
         if (import_handle == INVALID_HANDLE_VALUE)
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

         if (!DuplicateHandle(GetCurrentProcess(), import_handle, GetCurrentProcess(),
                              &dup_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }

      const VkImportSemaphoreWin32HandleInfoKHR win32_info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
         .semaphore = import_info->semaphore,
         .flags = import_info->flags,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
         .handle = dup_handle,
         .name = NULL,
      };
      const VkResult result =
         dev->ImportSemaphoreWin32HandleKHR(dev->base.handle.device, &win32_info);
      if (dup_handle != INVALID_HANDLE_VALUE)
         CloseHandle(dup_handle);
      return result;
   }
#endif

   return vk->ImportSemaphoreFdKHR(dev->base.handle.device, import_info);
}

static void
vkr_dispatch_vkCreateDevice(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCreateDevice *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   /* there can be at most two members sharing a queueFamilyIndex (one
    * protected-capable, one not), in which case their summed queueCount must
    * be <= the family's VkQueueFamilyProperties::queueCount.
    */
   {
      uint32_t *counts =
         malloc(sizeof(uint32_t) * physical_dev->queue_family_property_count);
      if (!counts) {
         args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
         return;
      }
      memset(counts, 0, sizeof(uint32_t) * physical_dev->queue_family_property_count);

      args->ret = VK_SUCCESS;
      for (uint32_t i = 0; i < args->pCreateInfo->queueCreateInfoCount; i++) {
         const uint32_t queue_family_index =
            args->pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
         const uint32_t queue_count = args->pCreateInfo->pQueueCreateInfos[i].queueCount;

         if (queue_family_index >= physical_dev->queue_family_property_count) {
            args->ret = VK_ERROR_UNKNOWN;
            break;
         }

         const VkQueueFamilyProperties *queue_family_properties =
            &physical_dev->queue_family_properties[queue_family_index];

         if (queue_family_properties->queueCount - counts[queue_family_index] <
             queue_count) {
            args->ret = VK_ERROR_UNKNOWN;
            break;
         }
         counts[queue_family_index] += queue_count;
      }

      free(counts);
      if (args->ret != VK_SUCCESS)
         return;
   }

   /* append extensions for our own use */
   const char **exts = NULL;
   uint32_t guest_ext_count = args->pCreateInfo->enabledExtensionCount;
   uint32_t ext_count = guest_ext_count + 6;
   exts = malloc(sizeof(*exts) * ext_count);
   if (!exts) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      return;
   }

   uint32_t host_ext_count = 0;
   for (uint32_t i = 0; i < guest_ext_count; i++) {
      const char *host_name = vkr_device_map_guest_ext_to_host(
         physical_dev, args->pCreateInfo->ppEnabledExtensionNames[i]);
      if (!vkr_device_has_extension(exts, host_ext_count, host_name))
         exts[host_ext_count++] = host_name;
   }

   if (physical_dev->host_external_memory_win32) {
      if (!vkr_device_has_extension(exts, host_ext_count, "VK_KHR_external_memory_win32"))
         exts[host_ext_count++] = "VK_KHR_external_memory_win32";
   } else if (physical_dev->KHR_external_memory_fd) {
      if (!vkr_device_has_extension(exts, host_ext_count, "VK_KHR_external_memory_fd"))
         exts[host_ext_count++] = "VK_KHR_external_memory_fd";
   }

   if (physical_dev->EXT_external_memory_dma_buf &&
       !vkr_device_has_extension(exts, host_ext_count, "VK_EXT_external_memory_dma_buf"))
      exts[host_ext_count++] = "VK_EXT_external_memory_dma_buf";

   if (physical_dev->host_external_fence_win32) {
      if (!vkr_device_has_extension(exts, host_ext_count, "VK_KHR_external_fence_win32"))
         exts[host_ext_count++] = "VK_KHR_external_fence_win32";
   } else if (physical_dev->KHR_external_fence_fd) {
      if (!vkr_device_has_extension(exts, host_ext_count, "VK_KHR_external_fence_fd"))
         exts[host_ext_count++] = "VK_KHR_external_fence_fd";
   }

   if (physical_dev->host_external_semaphore_win32) {
      if (!vkr_device_has_extension(exts, host_ext_count,
                                    "VK_KHR_external_semaphore_win32"))
         exts[host_ext_count++] = "VK_KHR_external_semaphore_win32";
   } else if (physical_dev->KHR_external_semaphore_fd) {
      if (!vkr_device_has_extension(exts, host_ext_count, "VK_KHR_external_semaphore_fd"))
         exts[host_ext_count++] = "VK_KHR_external_semaphore_fd";
   }

   ((VkDeviceCreateInfo *)args->pCreateInfo)->ppEnabledExtensionNames = exts;
   ((VkDeviceCreateInfo *)args->pCreateInfo)->enabledExtensionCount = host_ext_count;

   /* Ensure all queue families are requested on the host device.
    *
    * The guest may create a device with only a subset of queue families
    * (e.g. graphics only) but later create command pools for other families
    * (compute, transfer).  On Linux, some drivers tolerate this, but the
    * Windows Intel driver does not.  Add any missing queue families with
    * one queue each so the host device supports all of them.
    */
   VkDeviceQueueCreateInfo *all_queue_infos = NULL;
   float default_priority = 1.0f;
   {
      VkDeviceCreateInfo *ci = (VkDeviceCreateInfo *)args->pCreateInfo;
      const uint32_t family_count = physical_dev->queue_family_property_count;
      bool *requested = calloc(family_count, sizeof(bool));
      if (requested) {
         for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
            uint32_t idx = ci->pQueueCreateInfos[i].queueFamilyIndex;
            if (idx < family_count)
               requested[idx] = true;
         }

         uint32_t missing = 0;
         for (uint32_t i = 0; i < family_count; i++) {
            if (!requested[i])
               missing++;
         }

         if (missing) {
            uint32_t total = ci->queueCreateInfoCount + missing;
            all_queue_infos = calloc(total, sizeof(VkDeviceQueueCreateInfo));
            if (all_queue_infos) {
               memcpy(all_queue_infos, ci->pQueueCreateInfos,
                      ci->queueCreateInfoCount * sizeof(VkDeviceQueueCreateInfo));
               uint32_t idx = ci->queueCreateInfoCount;
               for (uint32_t i = 0; i < family_count; i++) {
                  if (!requested[i]) {
                     all_queue_infos[idx++] = (VkDeviceQueueCreateInfo){
                        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                        .queueFamilyIndex = i,
                        .queueCount = 1,
                        .pQueuePriorities = &default_priority,
                     };
                  }
               }
               ci->pQueueCreateInfos = all_queue_infos;
               ci->queueCreateInfoCount = total;
            }
         }
         free(requested);
      }
   }

   struct vkr_device *dev =
      vkr_context_alloc_object(ctx, sizeof(*dev), VK_OBJECT_TYPE_DEVICE, args->pDevice);
   if (!dev) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      free(exts);
      free(all_queue_infos);
      return;
   }

   vn_replace_vkCreateDevice_args_handle(args);
   args->ret = vk->CreateDevice(args->physicalDevice, args->pCreateInfo, NULL,
                                &dev->base.handle.device);

   if (args->ret != VK_SUCCESS) {
      free(exts);
      free(all_queue_infos);
      free(dev);
      return;
   }

   dev->physical_device = physical_dev;

   vkr_device_init_proc_table(dev, physical_dev->api_version,
                              args->pCreateInfo->ppEnabledExtensionNames,
                              args->pCreateInfo->enabledExtensionCount);

#ifdef _WIN32
   vkr_win32_device_install_shim(dev);
#endif

   free(exts);

   args->ret = vkr_device_create_queues(ctx, dev, args->pCreateInfo->queueCreateInfoCount,
                                        args->pCreateInfo->pQueueCreateInfos);
   free(all_queue_infos);
   if (args->ret != VK_SUCCESS) {
      struct vn_device_proc_table *vk = &dev->proc_table;
      vk->DestroyDevice(dev->base.handle.device, NULL);
      free(dev);
      return;
   }

   mtx_init(&dev->free_sync_mutex, mtx_plain);
   list_inithead(&dev->free_syncs);

   mtx_init(&dev->object_mutex, mtx_plain);
   list_inithead(&dev->objects);

   mtx_init(&dev->vk_api_mutex, mtx_plain);

   list_add(&dev->base.track_head, &physical_dev->devices);

   vkr_context_add_object(ctx, &dev->base);
}

static void
vkr_device_object_destroy(struct vkr_context *ctx,
                          struct vkr_device *dev,
                          struct vkr_object *obj)
{
   struct vn_device_proc_table *vk = &dev->proc_table;
   VkDevice device = dev->base.handle.device;

   assert(vkr_device_should_track_object(obj));

   if (ctx->on_worker_thread) {
      switch (obj->type) {
      case VK_OBJECT_TYPE_SEMAPHORE:
         vk->DestroySemaphore(device, obj->handle.semaphore, NULL);
         break;
      case VK_OBJECT_TYPE_FENCE:
         vk->DestroyFence(device, obj->handle.fence, NULL);
         break;
      case VK_OBJECT_TYPE_DEVICE_MEMORY:
         vk->FreeMemory(device, obj->handle.device_memory, NULL);
         break;
      case VK_OBJECT_TYPE_BUFFER:
         vk->DestroyBuffer(device, obj->handle.buffer, NULL);
         break;
      case VK_OBJECT_TYPE_IMAGE:
         vk->DestroyImage(device, obj->handle.image, NULL);
         break;
      case VK_OBJECT_TYPE_EVENT:
         vk->DestroyEvent(device, obj->handle.event, NULL);
         break;
      case VK_OBJECT_TYPE_QUERY_POOL:
         vk->DestroyQueryPool(device, obj->handle.query_pool, NULL);
         break;
      case VK_OBJECT_TYPE_BUFFER_VIEW:
         vk->DestroyBufferView(device, obj->handle.buffer_view, NULL);
         break;
      case VK_OBJECT_TYPE_IMAGE_VIEW:
         vk->DestroyImageView(device, obj->handle.image_view, NULL);
         break;
      case VK_OBJECT_TYPE_SHADER_MODULE:
         vk->DestroyShaderModule(device, obj->handle.shader_module, NULL);
         break;
      case VK_OBJECT_TYPE_PIPELINE_CACHE:
         vk->DestroyPipelineCache(device, obj->handle.pipeline_cache, NULL);
         break;
      case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
         vk->DestroyPipelineLayout(device, obj->handle.pipeline_layout, NULL);
         break;
      case VK_OBJECT_TYPE_RENDER_PASS:
         vk->DestroyRenderPass(device, obj->handle.render_pass, NULL);
         break;
      case VK_OBJECT_TYPE_PIPELINE:
         vk->DestroyPipeline(device, obj->handle.pipeline, NULL);
         break;
      case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
         vk->DestroyDescriptorSetLayout(device, obj->handle.descriptor_set_layout, NULL);
         break;
      case VK_OBJECT_TYPE_SAMPLER:
         vk->DestroySampler(device, obj->handle.sampler, NULL);
         break;
      case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
         vk->DestroyDescriptorPool(device, obj->handle.descriptor_pool, NULL);
         break;
      case VK_OBJECT_TYPE_FRAMEBUFFER:
         vk->DestroyFramebuffer(device, obj->handle.framebuffer, NULL);
         break;
      case VK_OBJECT_TYPE_COMMAND_POOL:
         vk->DestroyCommandPool(device, obj->handle.command_pool, NULL);
         break;
      case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION:
         vk->DestroySamplerYcbcrConversion(device, obj->handle.sampler_ycbcr_conversion,
                                           NULL);
         break;
      case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE:
         vk->DestroyDescriptorUpdateTemplate(
            device, obj->handle.descriptor_update_template, NULL);
         break;
      case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR:
         vk->DestroyAccelerationStructureKHR(device, obj->handle.acceleration_structure,
                                             NULL);
         break;
      default:
         vkr_log("Unhandled vkr_object(%p) with VkObjectType(%u)", obj,
                 (uint32_t)obj->type);
         assert(false);
         break;
      };
   }

   /* always cleanup vkr allocs */
   switch (obj->type) {
   case VK_OBJECT_TYPE_DEVICE_MEMORY:
      vkr_device_memory_release((struct vkr_device_memory *)obj);
      break;
   case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
      /* Destroying VkDescriptorPool frees all VkDescriptorSet allocated inside. */
      vkr_descriptor_pool_release(ctx, (struct vkr_descriptor_pool *)obj);
      break;
   case VK_OBJECT_TYPE_COMMAND_POOL:
      /* Destroying VkCommandPool frees all VkCommandBuffer allocated inside. */
      vkr_command_pool_release(ctx, (struct vkr_command_pool *)obj);
      break;
   default:
      break;
   };

   vkr_device_remove_object(ctx, dev, obj);
}

void
vkr_device_destroy(struct vkr_context *ctx, struct vkr_device *dev, bool destroy_vk)
{
   struct vn_device_proc_table *vk = &dev->proc_table;
   VkDevice device = dev->base.handle.device;

#ifdef _WIN32
   vkr_win32_device_remove_shim(dev);
#endif

   if (!list_is_empty(&dev->objects))
      vkr_log("destroying device with valid objects");

   /* only wait if on workder thread to prepare for vk obj cleanup */
   if (ctx->on_worker_thread) {
      VkResult result = vk->DeviceWaitIdle(device);
      if (result != VK_SUCCESS)
         vkr_log("vkDeviceWaitIdle(%p) failed(%d)", dev, (int32_t)result);
   }

   if (!list_is_empty(&dev->objects)) {
      list_for_each_entry_safe (struct vkr_object, obj, &dev->objects, track_head)
         vkr_device_object_destroy(ctx, dev, obj);
   }

#ifdef _WIN32
   /* Flush deferred object destructions.  The shim has been removed (proc
    * table restored to real functions), DeviceWaitIdle has completed, and
    * all tracked objects have been destroyed.  Now destroy the handles that
    * were kept alive to prevent Intel driver handle-reuse crashes.
    *
    * Only flush if on the worker thread (matching vkr_device_object_destroy
    * behavior).  Otherwise DestroyDevice will implicitly clean them up.
    */
   if (ctx->on_worker_thread)
      vkr_win32_flush_deferred_destroys(dev);
#endif

   mtx_destroy(&dev->object_mutex);

   list_for_each_entry_safe (struct vkr_queue, queue, &dev->queues, base.track_head)
      vkr_queue_destroy(ctx, queue);

   list_for_each_entry_safe (struct vkr_queue_sync, sync, &dev->free_syncs, head) {
      vk->DestroyFence(dev->base.handle.device, sync->fence, NULL);
      free(sync);
   }

   mtx_destroy(&dev->free_sync_mutex);
   mtx_destroy(&dev->vk_api_mutex);

   if (destroy_vk || ctx->on_worker_thread)
      vk->DestroyDevice(device, NULL);

   list_del(&dev->base.track_head);

   vkr_context_remove_object(ctx, &dev->base);
}

static void
vkr_dispatch_vkDestroyDevice(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkDestroyDevice *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = vkr_device_from_handle(args->device);
   /* this never happens */
   if (!dev)
      return;

   vkr_device_destroy(ctx, dev, true);
}

static void
vkr_dispatch_vkGetDeviceGroupPeerMemoryFeatures(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceGroupPeerMemoryFeatures *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceGroupPeerMemoryFeatures_args_handle(args);
   vk->GetDeviceGroupPeerMemoryFeatures(args->device, args->heapIndex,
                                        args->localDeviceIndex, args->remoteDeviceIndex,
                                        args->pPeerMemoryFeatures);
}

static void
vkr_dispatch_vkDeviceWaitIdle(struct vn_dispatch_context *dispatch,
                              UNUSED struct vn_command_vkDeviceWaitIdle *args)
{
   struct vkr_context *ctx = dispatch->data;
   /* no blocking call */
   vkr_context_set_fatal(ctx);
}

static void
vkr_dispatch_vkGetCalibratedTimestampsKHR(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetCalibratedTimestampsKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetCalibratedTimestampsKHR_args_handle(args);
   args->ret = vk->GetCalibratedTimestampsKHR(args->device, args->timestampCount,
                                              args->pTimestampInfos, args->pTimestamps,
                                              args->pMaxDeviation);
}

void
vkr_context_init_device_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateDevice = vkr_dispatch_vkCreateDevice;
   dispatch->dispatch_vkDestroyDevice = vkr_dispatch_vkDestroyDevice;
   dispatch->dispatch_vkGetDeviceProcAddr = NULL;
   dispatch->dispatch_vkGetDeviceGroupPeerMemoryFeatures =
      vkr_dispatch_vkGetDeviceGroupPeerMemoryFeatures;
   dispatch->dispatch_vkDeviceWaitIdle = vkr_dispatch_vkDeviceWaitIdle;
   dispatch->dispatch_vkGetCalibratedTimestampsKHR =
      vkr_dispatch_vkGetCalibratedTimestampsKHR;
}
