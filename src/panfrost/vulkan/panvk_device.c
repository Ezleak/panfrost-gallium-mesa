/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_device.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "panvk_device_memory.h"
#include "panvk_image.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_private.h"
#include "panvk_queue.h"

#include "decode.h"

#include "pan_encoder.h"
#include "pan_util.h"
#include "pan_props.h"
#include "pan_samples.h"

#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"

#include <fcntl.h>
#include <libsync.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>

#include "drm-uapi/panfrost_drm.h"

#include "util/disk_cache.h"
#include "util/strtod.h"
#include "util/u_debug.h"
#include "vk_drm_syncobj.h"
#include "vk_format.h"
#include "vk_util.h"

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include "wayland-drm-client-protocol.h"
#include <wayland-client.h>
#endif

#include "panvk_cs.h"

static void *
panvk_kmod_zalloc(const struct pan_kmod_allocator *allocator,
                  size_t size, bool transient)
{
   const VkAllocationCallbacks *vkalloc = allocator->priv;

   return vk_zalloc(vkalloc, size, 8,
                    transient ? VK_SYSTEM_ALLOCATION_SCOPE_COMMAND
                              : VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
}

static void
panvk_kmod_free(const struct pan_kmod_allocator *allocator, void *data)
{
   const VkAllocationCallbacks *vkalloc = allocator->priv;

   return vk_free(vkalloc, data);
}

struct panvk_priv_bo *panvk_priv_bo_create(struct panvk_device *dev,
                                           size_t size, uint32_t flags,
                                           const struct VkAllocationCallbacks *alloc,
                                           VkSystemAllocationScope scope)
{
   int ret;
   struct panvk_priv_bo *priv_bo =
      vk_zalloc2(&dev->vk.alloc, alloc, sizeof(*priv_bo), 8, scope);

   if (!priv_bo)
      return NULL;

   struct pan_kmod_bo *bo =
      pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, size, flags);
   if (!bo)
      goto err_free_priv_bo;

   priv_bo->bo = bo;
   priv_bo->dev = dev;

   if (!(flags & PAN_KMOD_BO_FLAG_NO_MMAP)) {
      priv_bo->addr.host = pan_kmod_bo_mmap(
         bo, 0, pan_kmod_bo_size(bo), PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
      if (priv_bo->addr.host == MAP_FAILED)
         goto err_put_bo;
   }

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = PAN_KMOD_VM_MAP_AUTO_VA,
         .size = pan_kmod_bo_size(bo),
      },
      .map = {
         .bo = priv_bo->bo,
         .bo_offset = 0,
      },
   };

   ret = pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   if (ret)
      goto err_munmap_bo;


   priv_bo->addr.dev = op.va.start;

   if (dev->debug.decode_ctx) {
      pandecode_inject_mmap(dev->debug.decode_ctx, priv_bo->addr.dev,
                            priv_bo->addr.host, pan_kmod_bo_size(priv_bo->bo),
                            NULL);
   }

   return priv_bo;

err_munmap_bo:
   if (priv_bo->addr.host) {
      ret = os_munmap(priv_bo->addr.host, pan_kmod_bo_size(bo));
      assert(!ret);
   }

err_put_bo:
   pan_kmod_bo_put(bo);

err_free_priv_bo:
   vk_free2(&dev->vk.alloc, alloc, priv_bo);
   return NULL;
}

void
panvk_priv_bo_destroy(struct panvk_priv_bo *priv_bo,
                      const VkAllocationCallbacks *alloc)
{
   if (!priv_bo)
      return;

   struct panvk_device *dev = priv_bo->dev;

   if (dev->debug.decode_ctx) {
      pandecode_inject_free(dev->debug.decode_ctx, priv_bo->addr.dev,
                            pan_kmod_bo_size(priv_bo->bo));
   }

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
      .va = {
         .start = priv_bo->addr.dev,
         .size = pan_kmod_bo_size(priv_bo->bo),
      },
   };
   ASSERTED int ret =
      pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   assert(!ret);

   if (priv_bo->addr.host) {
      ret = os_munmap(priv_bo->addr.host, pan_kmod_bo_size(priv_bo->bo));
      assert(!ret);
   }

   pan_kmod_bo_put(priv_bo->bo);
   vk_free2(&dev->vk.alloc, alloc, priv_bo);
}

/* Always reserve the lower 32MB. */
#define PANVK_VA_RESERVE_BOTTOM 0x2000000ull

VkResult panvk_v6_queue_init(struct panvk_device *device,
                             struct panvk_queue *queue, int idx,
                             const VkDeviceQueueCreateInfo *create_info);
VkResult panvk_v7_queue_init(struct panvk_device *device,
                             struct panvk_queue *queue, int idx,
                             const VkDeviceQueueCreateInfo *create_info);

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(panvk_physical_device, physical_device, physicalDevice);
   struct panvk_instance *instance = physical_device->instance;
   VkResult result;
   struct panvk_device *device;

   device = vk_zalloc2(&physical_device->instance->vk.alloc, pAllocator,
                       sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const struct vk_device_entrypoint_table *dev_entrypoints;
   const struct vk_command_buffer_ops *cmd_buffer_ops;
   struct vk_device_dispatch_table dispatch_table;
   unsigned arch = pan_arch(physical_device->kmod.props.gpu_prod_id);
   VkResult (*qinit)(struct panvk_device *, struct panvk_queue *, int,
                     const VkDeviceQueueCreateInfo *);

   switch (arch) {
   case 6:
      dev_entrypoints = &panvk_v6_device_entrypoints;
      cmd_buffer_ops = &panvk_v6_cmd_buffer_ops;
      qinit = panvk_v6_queue_init;
      break;
   case 7:
      dev_entrypoints = &panvk_v7_device_entrypoints;
      cmd_buffer_ops = &panvk_v7_cmd_buffer_ops;
      qinit = panvk_v7_queue_init;
      break;
   default:
      unreachable("Unsupported architecture");
   }

   /* For secondary command buffer support, overwrite any command entrypoints
    * in the main device-level dispatch table with
    * vk_cmd_enqueue_unless_primary_Cmd*.
    */
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_cmd_enqueue_unless_primary_device_entrypoints, true);

   vk_device_dispatch_table_from_entrypoints(&dispatch_table, dev_entrypoints,
                                             false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &panvk_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &wsi_device_entrypoints, false);

   /* Populate our primary cmd_dispatch table. */
   vk_device_dispatch_table_from_entrypoints(&device->cmd_dispatch,
                                             dev_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&device->cmd_dispatch,
                                             &panvk_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &device->cmd_dispatch, &vk_common_device_entrypoints, false);

   result = vk_device_init(&device->vk, &physical_device->vk, &dispatch_table,
                           pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return result;
   }

   /* Must be done after vk_device_init() because this function memset(0) the
    * whole struct.
    */
   device->vk.command_dispatch_table = &device->cmd_dispatch;
   device->vk.command_buffer_ops = cmd_buffer_ops;

   device->instance = physical_device->instance;
   device->physical_device = physical_device;

   device->kmod.allocator = (struct pan_kmod_allocator){
      .zalloc = panvk_kmod_zalloc,
      .free = panvk_kmod_free,
      .priv = &device->vk.alloc,
   };
   device->kmod.dev =
      pan_kmod_dev_create(dup(physical_device->kmod.dev->fd),
                          PAN_KMOD_DEV_FLAG_OWNS_FD, &device->kmod.allocator);

   if (instance->debug_flags & PANVK_DEBUG_TRACE)
      device->debug.decode_ctx = pandecode_create_context(false);

   /* 32bit address space, with the lower 32MB reserved. We clamp
    * things so it matches kmod VA range limitations.
    */
   uint64_t user_va_start = panfrost_clamp_to_usable_va_range(
      device->kmod.dev, PANVK_VA_RESERVE_BOTTOM);
   uint64_t user_va_end =
      panfrost_clamp_to_usable_va_range(device->kmod.dev, 1ull << 32);

   device->kmod.vm =
      pan_kmod_vm_create(device->kmod.dev, PAN_KMOD_VM_FLAG_AUTO_VA,
                         user_va_start, user_va_end - user_va_start);

   device->tiler_heap = panvk_priv_bo_create(
      device, 128 * 1024 * 1024,
      PAN_KMOD_BO_FLAG_NO_MMAP | PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT,
      &device->vk.alloc, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   device->sample_positions = panvk_priv_bo_create(
      device, panfrost_sample_positions_buffer_size(), 0,
      &device->vk.alloc, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   panfrost_upload_sample_positions(device->sample_positions->addr.host);

   vk_device_set_drm_fd(&device->vk, device->kmod.dev->fd);

   panvk_arch_dispatch(arch, meta_init, device);

   for (unsigned i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queue_create =
         &pCreateInfo->pQueueCreateInfos[i];
      uint32_t qfi = queue_create->queueFamilyIndex;
      device->queues[qfi] =
         vk_alloc(&device->vk.alloc,
                  queue_create->queueCount * sizeof(struct panvk_queue), 8,
                  VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!device->queues[qfi]) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      memset(device->queues[qfi], 0,
             queue_create->queueCount * sizeof(struct panvk_queue));

      device->queue_count[qfi] = queue_create->queueCount;

      for (unsigned q = 0; q < queue_create->queueCount; q++) {
         result = qinit(device, &device->queues[qfi][q], q, queue_create);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   *pDevice = panvk_device_to_handle(device);
   return VK_SUCCESS;

fail:
   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         panvk_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_object_free(&device->vk, NULL, device->queues[i]);
   }

   panvk_arch_dispatch(pan_arch(physical_device->kmod.props.gpu_prod_id),
                       meta_cleanup, device);
   panvk_priv_bo_destroy(device->tiler_heap, &device->vk.alloc);
   panvk_priv_bo_destroy(device->sample_positions, &device->vk.alloc);
   pan_kmod_vm_destroy(device->kmod.vm);
   pan_kmod_dev_destroy(device->kmod.dev);

   vk_free(&device->vk.alloc, device);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device = device->physical_device;

   if (!device)
      return;

   if (device->debug.decode_ctx)
      pandecode_destroy_context(device->debug.decode_ctx);

   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         panvk_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_object_free(&device->vk, NULL, device->queues[i]);
   }

   panvk_arch_dispatch(pan_arch(physical_device->kmod.props.gpu_prod_id),
                       meta_cleanup, device);
   panvk_priv_bo_destroy(device->tiler_heap, &device->vk.alloc);
   panvk_priv_bo_destroy(device->sample_positions, &device->vk.alloc);
   pan_kmod_vm_destroy(device->kmod.vm);
   pan_kmod_dev_destroy(device->kmod.dev);
   vk_free(&device->vk.alloc, device);
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetImageMemoryRequirements2(VkDevice device,
                                  const VkImageMemoryRequirementsInfo2 *pInfo,
                                  VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(panvk_image, image, pInfo->image);

   const uint64_t align = 4096;
   const uint64_t size = panvk_image_get_total_size(image);

   pMemoryRequirements->memoryRequirements.memoryTypeBits = 1;
   pMemoryRequirements->memoryRequirements.alignment = align;
   pMemoryRequirements->memoryRequirements.size = size;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetImageSparseMemoryRequirements2(
   VkDevice device, const VkImageSparseMemoryRequirementsInfo2 *pInfo,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   panvk_stub();
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_BindImageMemory2(VkDevice device, uint32_t bindInfoCount,
                       const VkBindImageMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(panvk_image, image, pBindInfos[i].image);
      VK_FROM_HANDLE(panvk_device_memory, mem, pBindInfos[i].memory);
      struct pan_kmod_bo *old_bo = image->bo;

      if (mem) {
         image->bo = pan_kmod_bo_get(mem->bo);
         image->pimage.data.base = mem->addr.dev;
         image->pimage.data.offset = pBindInfos[i].memoryOffset;
         /* Reset the AFBC headers */
         if (drm_is_afbc(image->pimage.layout.modifier)) {
            /* Transient CPU mapping */
            void *base = pan_kmod_bo_mmap(mem->bo, 0, pan_kmod_bo_size(mem->bo),
                                          PROT_WRITE, MAP_SHARED, NULL);

            assert(base != MAP_FAILED);

            for (unsigned layer = 0; layer < image->pimage.layout.array_size;
                 layer++) {
               for (unsigned level = 0; level < image->pimage.layout.nr_slices;
                    level++) {
                  void *header = base + image->pimage.data.offset +
                                 (layer * image->pimage.layout.array_stride) +
                                 image->pimage.layout.slices[level].offset;
                  memset(header, 0,
                         image->pimage.layout.slices[level].afbc.header_size);
               }
            }

            ASSERTED int ret = os_munmap(base, pan_kmod_bo_size(mem->bo));
            assert(!ret);
         }
      } else {
         image->bo = NULL;
         image->pimage.data.offset = pBindInfos[i].memoryOffset;
      }

      pan_kmod_bo_put(old_bo);
   }

   return VK_SUCCESS;
}
