/*
 * Copyright 2026 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * Unified Windows external memory/sync translation shim for Venus.
 *
 * On Linux, Venus uses fd-based external memory and sync primitives.
 * On Windows, the host Vulkan driver exposes Win32 handle equivalents.
 * This shim intercepts the relevant Vulkan API calls at the proc table
 * level and translates fd semantics to Win32 handles transparently.
 *
 * It replaces the piecemeal translations previously scattered across
 * vkr_buffer.c, vkr_device.c, vkr_queue.c, and vkr_device_memory.c.
 */

#ifndef VKR_WIN32_SHIM_H
#define VKR_WIN32_SHIM_H

#ifdef _WIN32

struct vkr_device;

/*
 * Install Win32 translation shims into the device proc table.
 * Must be called after vkr_device_init_proc_table and after the
 * Win32 function pointers (GetMemoryWin32HandleKHR etc.) are loaded.
 *
 * Replaces these proc table entries with wrappers:
 *   - CreateBuffer      (adds VkExternalMemoryBufferCreateInfo)
 *   - CreateImage       (adds VkExternalMemoryImageCreateInfo)
 *   - GetMemoryFdKHR    (translates to GetMemoryWin32HandleKHR)
 *   - GetFenceFdKHR     (translates to GetFenceWin32HandleKHR)
 *   - GetSemaphoreFdKHR (translates to GetSemaphoreWin32HandleKHR)
 *   - ImportFenceFdKHR  (translates to ImportFenceWin32HandleKHR)
 *   - ImportSemaphoreFdKHR (translates to ImportSemaphoreWin32HandleKHR)
 *
 * Also installs deferred-destroy shims for all non-dispatchable object
 * types to prevent Intel driver crashes from handle value reuse.
 */
void
vkr_win32_device_install_shim(struct vkr_device *dev);

/*
 * Unregister a device from the shim's device map and restore the
 * original proc table entries.
 * Must be called before the device is destroyed.
 */
void
vkr_win32_device_remove_shim(struct vkr_device *dev);

/*
 * Destroy all Vulkan objects whose destruction was deferred by the shim.
 * Must be called after DeviceWaitIdle (no in-flight work) and after
 * the shim has been removed (proc table restored to real functions).
 */
void
vkr_win32_flush_deferred_destroys(struct vkr_device *dev);

#endif /* _WIN32 */

#endif /* VKR_WIN32_SHIM_H */
