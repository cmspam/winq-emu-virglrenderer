/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_IMAGE_H
#define VKR_IMAGE_H

#include "vkr_common.h"

struct vkr_image {
   struct vkr_object base;

#ifdef _WIN32
   /* Set when the image was created through the VK_EXT_image_drm_format_modifier
    * shim: the guest asked for tiling=DRM_FORMAT_MODIFIER_EXT with a
    * modifier pNext chain; the shim rewrote that to tiling=LINEAR before
    * forwarding to the host ICD. Used later to translate aspect-mask
    * values in vkGetImageSubresourceLayout(2) — the guest queries
    * VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT (valid only for MODIFIER
    * tiling), but on the host we need VK_IMAGE_ASPECT_COLOR_BIT (the
    * aspect valid for LINEAR). */
   bool dma_buf_shim_linear;
#endif
};
VKR_DEFINE_OBJECT_CAST(image, VK_OBJECT_TYPE_IMAGE, VkImage)

struct vkr_image_view {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(image_view, VK_OBJECT_TYPE_IMAGE_VIEW, VkImageView)

struct vkr_sampler {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(sampler, VK_OBJECT_TYPE_SAMPLER, VkSampler)

struct vkr_sampler_ycbcr_conversion {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(sampler_ycbcr_conversion,
                       VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION,
                       VkSamplerYcbcrConversion)

void
vkr_context_init_image_dispatch(struct vkr_context *ctx);

void
vkr_context_init_image_view_dispatch(struct vkr_context *ctx);

void
vkr_context_init_sampler_dispatch(struct vkr_context *ctx);

void
vkr_context_init_sampler_ycbcr_conversion_dispatch(struct vkr_context *ctx);

#endif /* VKR_IMAGE_H */
