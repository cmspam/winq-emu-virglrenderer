/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_PHYSICAL_DEVICE_H
#define VKR_PHYSICAL_DEVICE_H

#include "vkr_common.h"

#include "venus-protocol/vn_protocol_renderer_util.h"

struct vkr_physical_device {
   struct vkr_object base;

   struct vn_physical_device_proc_table proc_table;

   VkPhysicalDeviceProperties properties;
   uint32_t api_version;

   VkExtensionProperties *extensions;
   uint32_t extension_count;

   bool KHR_external_memory_fd;
   bool EXT_external_memory_dma_buf;

   bool KHR_external_fence_fd;
   bool KHR_external_semaphore_fd;

   bool host_external_memory_win32;
   bool host_external_fence_win32;
   bool host_external_semaphore_win32;

   /* Windows-host synthesis of Linux dma-buf + modifier extensions.
    * When true, vkr advertises VK_EXT_external_memory_dma_buf and
    * VK_EXT_image_drm_format_modifier to the guest and internally
    * translates them to OPAQUE_WIN32 + LINEAR tiling on the host ICD.
    *
    * This unblocks guest stacks that require these extensions to
    * function: ANGLE's Vulkan backend for Chromium's VaapiVideoDecoder,
    * Zink's dma-buf export for Wayland presentation, and any Mesa
    * consumer that checks for dma-buf Vulkan interop before taking
    * the accelerated path.
    */
   bool dma_buf_shim_active;
   /* True when the extension was advertised by vkr but not by the host
    * ICD. CreateDevice must NOT pass these names to the host ICD.
    */
   bool EXT_external_memory_dma_buf_synthesized;
   bool EXT_image_drm_format_modifier_synthesized;

   VkPhysicalDeviceMemoryProperties memory_properties;
   VkPhysicalDeviceIDProperties id_properties;
   bool is_dma_buf_fd_export_supported;
   bool is_opaque_fd_export_supported;
   void *gbm_device;
   int udmabuf_dev_fd;

   VkQueueFamilyProperties *queue_family_properties;
   uint32_t queue_family_property_count;

   struct list_head devices;
};
VKR_DEFINE_OBJECT_CAST(physical_device, VK_OBJECT_TYPE_PHYSICAL_DEVICE, VkPhysicalDevice)

void
vkr_context_init_physical_device_dispatch(struct vkr_context *ctx);

void
vkr_physical_device_destroy(struct vkr_context *ctx,
                            struct vkr_physical_device *physical_dev);

#endif /* VKR_PHYSICAL_DEVICE_H */
