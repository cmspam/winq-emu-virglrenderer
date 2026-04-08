/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_H
#define VKR_DEVICE_H

#include "vkr_common.h"

#include "venus-protocol/vn_protocol_renderer_util.h"

#ifdef _WIN32
#include <vulkan/vulkan_win32.h>

/* Entry for deferred Vulkan object destruction on Windows.
 *
 * The Intel Windows Vulkan driver crashes when a non-dispatchable handle value
 * (e.g. VkDescriptorSetLayout) is reused after the previous object with that
 * value was destroyed.  The driver's internal handle cache retains stale data.
 *
 * We work around this by deferring driver-level destruction: when the guest
 * destroys an object, we remove it from Venus tracking but do NOT call the
 * driver's vkDestroy*.  The driver handle stays alive, preventing reuse.
 * All deferred handles are flushed (actually destroyed) at device teardown,
 * after DeviceWaitIdle guarantees no in-flight work references them.
 */
struct vkr_win32_deferred_handle {
   VkObjectType type;
   uint64_t handle;
};
#endif

#include "vkr_context.h"

struct vkr_device {
   struct vkr_object base;

   struct vkr_physical_device *physical_device;

   struct vn_device_proc_table proc_table;

#ifdef _WIN32
   PFN_vkGetMemoryWin32HandleKHR GetMemoryWin32HandleKHR;
   PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR;
   PFN_vkGetFenceWin32HandleKHR GetFenceWin32HandleKHR;
   PFN_vkImportFenceWin32HandleKHR ImportFenceWin32HandleKHR;
   PFN_vkGetSemaphoreWin32HandleKHR GetSemaphoreWin32HandleKHR;
   PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR;

   /* Deferred object destruction — see struct vkr_win32_deferred_handle */
   struct vkr_win32_deferred_handle *deferred_destroys;
   uint32_t deferred_destroy_count;
   uint32_t deferred_destroy_capacity;
#endif

   struct list_head queues;

   mtx_t free_sync_mutex;
   struct list_head free_syncs;

   mtx_t object_mutex;
   struct list_head objects;

   mtx_t vk_api_mutex;
};
VKR_DEFINE_OBJECT_CAST(device, VK_OBJECT_TYPE_DEVICE, VkDevice)

void
vkr_context_init_device_dispatch(struct vkr_context *ctx);

void
vkr_device_destroy(struct vkr_context *ctx, struct vkr_device *dev, bool destroy_vk);

VkResult
vkr_device_get_fence_fd(struct vkr_device *dev,
                        VkFence fence,
                        VkExternalFenceHandleTypeFlagBits handle_type,
                        int *out_fd);

VkResult
vkr_device_get_semaphore_fd(struct vkr_device *dev,
                            VkSemaphore semaphore,
                            VkExternalSemaphoreHandleTypeFlagBits handle_type,
                            int *out_fd);

VkResult
vkr_device_import_fence_fd(struct vkr_device *dev,
                           const VkImportFenceFdInfoKHR *import_info);

VkResult
vkr_device_import_semaphore_fd(struct vkr_device *dev,
                               const VkImportSemaphoreFdInfoKHR *import_info);

static inline bool
vkr_device_should_track_object(const struct vkr_object *obj)
{
   assert(vkr_is_recognized_object_type(obj->type));

   switch (obj->type) {
   case VK_OBJECT_TYPE_INSTANCE:        /* non-device objects */
   case VK_OBJECT_TYPE_PHYSICAL_DEVICE: /* non-device objects */
   case VK_OBJECT_TYPE_DEVICE:          /* device itself */
   case VK_OBJECT_TYPE_QUEUE:           /* not tracked as device objects */
   case VK_OBJECT_TYPE_COMMAND_BUFFER:  /* pool objects */
   case VK_OBJECT_TYPE_DESCRIPTOR_SET:  /* pool objects */
      return false;
   default:
      return true;
   }
}

static inline void
vkr_device_add_object(struct vkr_context *ctx,
                      struct vkr_device *dev,
                      struct vkr_object *obj)
{
   vkr_context_add_object(ctx, obj);

   assert(vkr_device_should_track_object(obj));

   mtx_lock(&dev->object_mutex);
   list_add(&obj->track_head, &dev->objects);
   mtx_unlock(&dev->object_mutex);
}

static inline void
vkr_device_remove_object(struct vkr_context *ctx,
                         UNUSED struct vkr_device *dev,
                         struct vkr_object *obj)
{
   assert(vkr_device_should_track_object(obj));

   mtx_lock(&dev->object_mutex);
   list_del(&obj->track_head);
   mtx_unlock(&dev->object_mutex);

   /* this frees obj */
   vkr_context_remove_object(ctx, obj);
}

static inline void
vkr_device_lock_api(struct vkr_device *dev)
{
   mtx_lock(&dev->vk_api_mutex);
}

static inline void
vkr_device_unlock_api(struct vkr_device *dev)
{
   mtx_unlock(&dev->vk_api_mutex);
}

#endif /* VKR_DEVICE_H */
