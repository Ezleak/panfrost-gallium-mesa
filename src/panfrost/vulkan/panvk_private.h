/*
 * Copyright © 2021 Collabora Ltd.
 *
 * derived from tu_private.h driver which is:
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

#ifndef PANVK_PRIVATE_H
#define PANVK_PRIVATE_H

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"
#include "compiler/shader_enums.h"
#include "util/list.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_descriptor_set_layout.h"
#include "vk_device.h"
#include "vk_image.h"
#include "vk_instance.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_physical_device.h"
#include "vk_pipeline_layout.h"
#include "vk_queue.h"
#include "vk_sync.h"
#include "wsi_common.h"

#include "drm-uapi/panfrost_drm.h"

#include "pan_blend.h"
#include "pan_blitter.h"
#include "pan_desc.h"
#include "pan_jc.h"
#include "pan_texture.h"
#include "panvk_descriptor_set.h"
#include "panvk_descriptor_set_layout.h"
#include "panvk_macros.h"
#include "panvk_mempool.h"
#include "panvk_pipeline.h"
#include "panvk_pipeline_layout.h"
#include "panvk_varyings.h"
#include "vk_extensions.h"

#include "kmod/pan_kmod.h"

/* Pre-declarations needed for WSI entrypoints */
struct wl_surface;
struct wl_display;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_window_t;

#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "panvk_entrypoints.h"

#define MAX_BIND_POINTS             2 /* compute + graphics */
#define MAX_VBS                     16
#define MAX_VERTEX_ATTRIBS          16
#define MAX_VSC_PIPES               32
#define MAX_VIEWPORTS               1
#define MAX_SCISSORS                16
#define MAX_DISCARD_RECTANGLES      4
#define MAX_PUSH_CONSTANTS_SIZE     128
#define MAX_PUSH_DESCRIPTORS        32
#define MAX_SAMPLES_LOG2 4
#define NUM_META_FS_KEYS 13
#define MAX_VIEWS        8

#define NUM_DEPTH_CLEAR_PIPELINES 3

#define PANVK_SYSVAL_UBO_INDEX     0
#define PANVK_PUSH_CONST_UBO_INDEX 1
#define PANVK_NUM_BUILTIN_UBOS     2

struct panvk_device;
struct panvk_pipeline_layout;

/* Used for internal object allocation. */
struct panvk_priv_bo {
   struct panvk_device *dev;
   struct pan_kmod_bo *bo;
   struct {
      mali_ptr dev;
      void *host;
   } addr;
};

struct panvk_priv_bo *panvk_priv_bo_create(struct panvk_device *dev,
                                           size_t size, uint32_t flags,
                                           const VkAllocationCallbacks *alloc,
                                           VkSystemAllocationScope scope);

void panvk_priv_bo_destroy(struct panvk_priv_bo *bo,
                           const VkAllocationCallbacks *alloc);

#define PANVK_META_COPY_BUF2IMG_NUM_FORMATS  12
#define PANVK_META_COPY_IMG2BUF_NUM_FORMATS  12
#define PANVK_META_COPY_IMG2IMG_NUM_FORMATS  14
#define PANVK_META_COPY_NUM_TEX_TYPES        5
#define PANVK_META_COPY_BUF2BUF_NUM_BLKSIZES 5

static inline unsigned
panvk_meta_copy_tex_type(unsigned dim, bool isarray)
{
   assert(dim > 0 && dim <= 3);
   assert(dim < 3 || !isarray);
   return (((dim - 1) << 1) | (isarray ? 1 : 0));
}

struct panvk_meta {

   struct panvk_pool bin_pool;
   struct panvk_pool desc_pool;

   /* Access to the blitter pools are protected by the blitter
    * shader/rsd locks. They can't be merged with other binary/desc
    * pools unless we patch pan_blitter.c to external pool locks.
    */
   struct {
      struct panvk_pool bin_pool;
      struct panvk_pool desc_pool;
      struct pan_blitter_cache cache;
   } blitter;

   struct pan_blend_shader_cache blend_shader_cache;

   struct {
      struct {
         mali_ptr shader;
         struct pan_shader_info shader_info;
      } color[3]; /* 3 base types */
   } clear_attachment;

   struct {
      struct {
         mali_ptr rsd;
      } buf2img[PANVK_META_COPY_BUF2IMG_NUM_FORMATS];
      struct {
         mali_ptr rsd;
      } img2buf[PANVK_META_COPY_NUM_TEX_TYPES]
               [PANVK_META_COPY_IMG2BUF_NUM_FORMATS];
      struct {
         mali_ptr rsd;
      } img2img[2][PANVK_META_COPY_NUM_TEX_TYPES]
               [PANVK_META_COPY_IMG2IMG_NUM_FORMATS];
      struct {
         mali_ptr rsd;
      } buf2buf[PANVK_META_COPY_BUF2BUF_NUM_BLKSIZES];
      struct {
         mali_ptr rsd;
      } fillbuf;
   } copy;
};

struct panvk_physical_device {
   struct vk_physical_device vk;

   struct {
      struct pan_kmod_dev *dev;
      struct pan_kmod_dev_props props;
   } kmod;

   const struct panfrost_model *model;
   struct {
      const struct pan_blendable_format *blendable;
      const struct panfrost_format *all;
   } formats;

   struct panvk_instance *instance;

   char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
   uint8_t driver_uuid[VK_UUID_SIZE];
   uint8_t device_uuid[VK_UUID_SIZE];
   uint8_t cache_uuid[VK_UUID_SIZE];

   struct vk_sync_type drm_syncobj_type;
   const struct vk_sync_type *sync_types[2];

   struct wsi_device wsi_device;

   int master_fd;
};

enum panvk_debug_flags {
   PANVK_DEBUG_STARTUP = 1 << 0,
   PANVK_DEBUG_NIR = 1 << 1,
   PANVK_DEBUG_TRACE = 1 << 2,
   PANVK_DEBUG_SYNC = 1 << 3,
   PANVK_DEBUG_AFBC = 1 << 4,
   PANVK_DEBUG_LINEAR = 1 << 5,
   PANVK_DEBUG_DUMP = 1 << 6,
   PANVK_DEBUG_NO_KNOWN_WARN = 1 << 7,
};

struct panvk_instance {
   struct vk_instance vk;

   uint32_t api_version;

   enum panvk_debug_flags debug_flags;

   struct {
      struct pan_kmod_allocator allocator;
   } kmod;
};

VkResult panvk_wsi_init(struct panvk_physical_device *physical_device);
void panvk_wsi_finish(struct panvk_physical_device *physical_device);

bool panvk_instance_extension_supported(const char *name);
uint32_t panvk_physical_device_api_version(struct panvk_physical_device *dev);
bool
panvk_physical_device_extension_supported(struct panvk_physical_device *dev,
                                          const char *name);

#define PANVK_MAX_QUEUE_FAMILIES 1

struct panvk_queue {
   struct vk_queue vk;
   struct panvk_device *device;
   uint32_t sync;
};

struct panvk_device {
   struct vk_device vk;

   struct {
      struct pan_kmod_vm *vm;
      struct pan_kmod_dev *dev;
      struct pan_kmod_allocator allocator;
   } kmod;

   struct panvk_priv_bo *tiler_heap;
   struct panvk_priv_bo *sample_positions;

   struct panvk_meta meta;

   struct vk_device_dispatch_table cmd_dispatch;

   struct panvk_instance *instance;

   struct panvk_queue *queues[PANVK_MAX_QUEUE_FAMILIES];
   int queue_count[PANVK_MAX_QUEUE_FAMILIES];

   struct panvk_physical_device *physical_device;

   struct {
      struct pandecode_context *decode_ctx;
   } debug;
};

#define TILER_DESC_WORDS 56

struct panvk_batch {
   struct list_head node;
   struct util_dynarray jobs;
   struct util_dynarray event_ops;
   struct pan_jc jc;
   struct {
      struct panfrost_ptr desc;
      uint32_t bo_count;
      struct pan_kmod_bo *bos[MAX_RTS + 2];
   } fb;
   struct {
      struct pan_kmod_bo *src, *dst;
   } blit;
   struct panfrost_ptr tls;
   mali_ptr fragment_job;
   struct {
      struct pan_tiler_context ctx;
      struct panfrost_ptr descs;
      uint32_t templ[TILER_DESC_WORDS];
   } tiler;
   struct pan_tls_info tlsinfo;
   unsigned wls_total_size;
   bool issued;
};

enum panvk_cmd_event_op_type {
   PANVK_EVENT_OP_SET,
   PANVK_EVENT_OP_RESET,
   PANVK_EVENT_OP_WAIT,
};

struct panvk_cmd_event_op {
   enum panvk_cmd_event_op_type type;
   struct panvk_event *event;
};

enum panvk_dynamic_state_bits {
   PANVK_DYNAMIC_VIEWPORT = 1 << 0,
   PANVK_DYNAMIC_SCISSOR = 1 << 1,
   PANVK_DYNAMIC_LINE_WIDTH = 1 << 2,
   PANVK_DYNAMIC_DEPTH_BIAS = 1 << 3,
   PANVK_DYNAMIC_BLEND_CONSTANTS = 1 << 4,
   PANVK_DYNAMIC_DEPTH_BOUNDS = 1 << 5,
   PANVK_DYNAMIC_STENCIL_COMPARE_MASK = 1 << 6,
   PANVK_DYNAMIC_STENCIL_WRITE_MASK = 1 << 7,
   PANVK_DYNAMIC_STENCIL_REFERENCE = 1 << 8,
   PANVK_DYNAMIC_DISCARD_RECTANGLE = 1 << 9,
   PANVK_DYNAMIC_SSBO = 1 << 10,
   PANVK_DYNAMIC_VERTEX_INSTANCE_OFFSETS = 1 << 11,
   PANVK_DYNAMIC_ALL = (1 << 12) - 1,
};

union panvk_sysval_vec4 {
   float f32[4];
   uint32_t u32[4];
};

struct panvk_sysvals {
   union {
      struct {
         /* Only for graphics */
         union panvk_sysval_vec4 viewport_scale;
         union panvk_sysval_vec4 viewport_offset;
         union panvk_sysval_vec4 blend_constants;

         uint32_t first_vertex;
         uint32_t base_vertex;
         uint32_t base_instance;
      };

      struct {
         /* Only for compute */
         union panvk_sysval_vec4 num_work_groups;
         union panvk_sysval_vec4 local_group_size;
      };
   };

   /* The back-end compiler doesn't know about any sysvals after this point */

   struct panvk_ssbo_addr dyn_ssbos[MAX_DYNAMIC_STORAGE_BUFFERS];
};

struct panvk_descriptor_state {
   uint32_t dirty;
   const struct panvk_descriptor_set *sets[MAX_SETS];
   struct panvk_sysvals sysvals;
   struct {
      struct panvk_buffer_desc ubos[MAX_DYNAMIC_UNIFORM_BUFFERS];
      struct panvk_buffer_desc ssbos[MAX_DYNAMIC_STORAGE_BUFFERS];
   } dyn;
   mali_ptr sysvals_ptr;
   mali_ptr ubos;
   mali_ptr textures;
   mali_ptr samplers;
   mali_ptr push_constants;
   mali_ptr vs_attribs;
   mali_ptr vs_attrib_bufs;
   mali_ptr non_vs_attribs;
   mali_ptr non_vs_attrib_bufs;
};

#define INVOCATION_DESC_WORDS 2

struct panvk_draw_info {
   unsigned first_index;
   unsigned index_count;
   unsigned index_size;
   unsigned first_vertex;
   unsigned vertex_count;
   unsigned vertex_range;
   unsigned padded_vertex_count;
   unsigned first_instance;
   unsigned instance_count;
   int vertex_offset;
   unsigned offset_start;
   uint32_t invocation[INVOCATION_DESC_WORDS];
   struct {
      mali_ptr varyings;
      mali_ptr attributes;
      mali_ptr attribute_bufs;
      mali_ptr push_constants;
   } stages[MESA_SHADER_STAGES];
   mali_ptr varying_bufs;
   mali_ptr textures;
   mali_ptr samplers;
   mali_ptr ubos;
   mali_ptr position;
   mali_ptr indices;
   union {
      mali_ptr psiz;
      float line_width;
   };
   mali_ptr tls;
   mali_ptr fb;
   const struct pan_tiler_context *tiler_ctx;
   mali_ptr fs_rsd;
   mali_ptr viewport;
   struct {
      struct panfrost_ptr vertex;
      struct panfrost_ptr tiler;
   } jobs;
};

struct panvk_dispatch_info {
   struct pan_compute_dim wg_count;
   mali_ptr attributes;
   mali_ptr attribute_bufs;
   mali_ptr tsd;
   mali_ptr ubos;
   mali_ptr push_uniforms;
   mali_ptr textures;
   mali_ptr samplers;
};

struct panvk_attrib_buf {
   mali_ptr address;
   unsigned size;
};

struct panvk_cmd_state {
   uint32_t dirty;

   struct panvk_varyings_info varyings;
   mali_ptr fs_rsd;

   struct {
      float constants[4];
   } blend;

   struct {
      struct {
         float constant_factor;
         float clamp;
         float slope_factor;
      } depth_bias;
      float line_width;
   } rast;

   struct {
      struct panvk_attrib_buf bufs[MAX_VBS];
      unsigned count;
   } vb;

   /* Index buffer */
   struct {
      struct panvk_buffer *buffer;
      uint64_t offset;
      uint8_t index_size;
      uint32_t first_vertex, base_vertex, base_instance;
   } ib;

   struct {
      struct {
         uint8_t compare_mask;
         uint8_t write_mask;
         uint8_t ref;
      } s_front, s_back;
   } zs;

   struct {
      struct pan_fb_info info;
      bool crc_valid[MAX_RTS];
      uint32_t bo_count;
      struct pan_kmod_bo *bos[MAX_RTS + 2];
   } fb;

   mali_ptr vpd;
   VkViewport viewport;
   VkRect2D scissor;

   struct panvk_batch *batch;
};

struct panvk_cmd_pool {
   struct vk_command_pool vk;
   struct panvk_bo_pool desc_bo_pool;
   struct panvk_bo_pool varying_bo_pool;
   struct panvk_bo_pool tls_bo_pool;
};

struct panvk_cmd_bind_point_state {
   struct panvk_descriptor_state desc_state;
   const struct panvk_pipeline *pipeline;
};

struct panvk_cmd_buffer {
   struct vk_command_buffer vk;

   struct panvk_device *device;

   struct panvk_pool desc_pool;
   struct panvk_pool varying_pool;
   struct panvk_pool tls_pool;
   struct list_head batches;

   VkCommandBufferUsageFlags usage_flags;

   struct panvk_cmd_state state;

   uint8_t push_constants[MAX_PUSH_CONSTANTS_SIZE];
   VkShaderStageFlags push_constant_stages;
   struct panvk_descriptor_set meta_push_descriptors;

   struct panvk_cmd_bind_point_state bind_points[MAX_BIND_POINTS];
};

#define panvk_cmd_get_bind_point_state(cmdbuf, bindpoint)                      \
   &(cmdbuf)->bind_points[VK_PIPELINE_BIND_POINT_##bindpoint]

#define panvk_cmd_get_pipeline(cmdbuf, bindpoint)                              \
   (cmdbuf)->bind_points[VK_PIPELINE_BIND_POINT_##bindpoint].pipeline

#define panvk_cmd_get_desc_state(cmdbuf, bindpoint)                            \
   &(cmdbuf)->bind_points[VK_PIPELINE_BIND_POINT_##bindpoint].desc_state

struct panvk_batch *panvk_cmd_open_batch(struct panvk_cmd_buffer *cmdbuf);

void panvk_cmd_preload_fb_after_batch_split(struct panvk_cmd_buffer *cmdbuf);

struct panvk_shader {
   struct pan_shader_info info;
   struct util_dynarray binary;
   unsigned sysval_ubo;
   struct pan_compute_dim local_size;
   bool has_img_access;
};

struct panvk_shader *
panvk_shader_create(struct panvk_device *dev, gl_shader_stage stage,
                    const VkPipelineShaderStageCreateInfo *stage_info,
                    const struct panvk_pipeline_layout *layout,
                    unsigned sysval_ubo, struct pan_blend_state *blend_state,
                    bool static_blend_constants,
                    const VkAllocationCallbacks *alloc);

void panvk_shader_destroy(struct panvk_device *dev, struct panvk_shader *shader,
                          const VkAllocationCallbacks *alloc);

VK_DEFINE_HANDLE_CASTS(panvk_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)
VK_DEFINE_HANDLE_CASTS(panvk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)
VK_DEFINE_HANDLE_CASTS(panvk_instance, vk.base, VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)
VK_DEFINE_HANDLE_CASTS(panvk_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)
VK_DEFINE_HANDLE_CASTS(panvk_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_cmd_pool, vk.base, VkCommandPool,
                               VK_OBJECT_TYPE_COMMAND_POOL)

#ifdef PAN_ARCH
#include "panvk_vX_cmd_buffer.h"
#include "panvk_vX_cs.h"
#include "panvk_vX_device.h"
#include "panvk_vX_meta.h"
#else
#define PAN_ARCH             6
#define panvk_per_arch(name) panvk_arch_name(name, v6)
#include "panvk_vX_cmd_buffer.h"
#include "panvk_vX_cs.h"
#include "panvk_vX_device.h"
#include "panvk_vX_meta.h"
#undef PAN_ARCH
#undef panvk_per_arch
#define PAN_ARCH             7
#define panvk_per_arch(name) panvk_arch_name(name, v7)
#include "panvk_vX_cmd_buffer.h"
#include "panvk_vX_cs.h"
#include "panvk_vX_device.h"
#include "panvk_vX_meta.h"
#undef PAN_ARCH
#undef panvk_per_arch
#endif

#ifdef PAN_ARCH
bool panvk_per_arch(blend_needs_lowering)(const struct panvk_device *dev,
                                          const struct pan_blend_state *state,
                                          unsigned rt);

struct panvk_shader *panvk_per_arch(shader_create)(
   struct panvk_device *dev, gl_shader_stage stage,
   const VkPipelineShaderStageCreateInfo *stage_info,
   const struct panvk_pipeline_layout *layout, unsigned sysval_ubo,
   struct pan_blend_state *blend_state, bool static_blend_constants,
   const VkAllocationCallbacks *alloc);
struct nir_shader;

bool panvk_per_arch(nir_lower_descriptors)(
   struct nir_shader *nir, struct panvk_device *dev,
   const struct panvk_pipeline_layout *layout, bool *has_img_access_out);
#endif

#endif /* PANVK_PRIVATE_H */
