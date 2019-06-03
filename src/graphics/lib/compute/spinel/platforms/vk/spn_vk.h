// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_SPN_VK_H_
#define SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_SPN_VK_H_

//
//
//

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#include "spn_vk_layouts.h"

//
// No need to know how these are implemented
//

struct spn_device;
struct spn_vk_environment;
struct spn_vk_target;

//
// If you want to see what's happening here with all of the descriptor
// layout expansions, run target.c through the preprocessor:
//
// clang -I %VULKAN_SDK%\include -I ..\.. -I ..\..\.. -E  spn_vk.c | clang-format > spn_vk_clang.c
// cl    -I %VULKAN_SDK%\include -I ..\.. -I ..\..\.. -EP spn_vk.c | clang-format > spn_vk_msvc.c
//

//
// Update the descriptor sets
//
// There are currently 10 descriptor sets:
//
//   - block_pool
//   - path copy
//   - fill_cmds
//   - prim_scan
//   - rast_cmds
//   - ttrks
//   - ttcks
//   - place_cmds
//   - styling
//   - surface
//
// Most descriptor sets are ephemeral and sized according to the
// target config.
//
// The following descriptor sets are durable and are either explicitly
// sized or sized using configuration defaults:
//
//   - block_pool
//   - fill_cmds
//   - place_cmds
//   - ttcks
//   - styling
//
// The surface descriptor set is currently the only descriptor that is
// externally defined/allocated/managed:
//
//   - surface
//

//
// Create an instance of the Spinel target
//

struct spn_vk *
spn_vk_create(struct spn_vk_environment * const  environment,
              struct spn_vk_target const * const target);

//
// Resources will be disposed of with the same device and allocator
// that was used for creation.
//

void
spn_vk_dispose(struct spn_vk * const instance, struct spn_vk_environment * const vk);

//
// Get the target configuration structure
//

struct spn_vk_target_config const *
spn_vk_get_config(struct spn_vk const * const instance);

//
// Declare host-side descriptor set buffer/image binding structures:
//
//   struct spn_vk_buf_block_pool_bp_ids
//   struct spn_vk_buf_block_pool_bp_blocks
//   ...
//   struct spn_vk_buf_render_surface
//

#define SPN_VK_TARGET_BUFFER_NAME(_ds_id, _name) struct spn_vk_buf_##_ds_id##_##_name

#define SPN_VK_TARGET_GLSL_LAYOUT_BUFFER(_ds_id, _s_idx, _b_idx, _name)                            \
  SPN_VK_TARGET_BUFFER_NAME(_ds_id, _name)

#define SPN_VK_TARGET_GLSL_LAYOUT_IMAGE2D(_ds_id, _s_idx, _b_idx, _img_type, _name)

SPN_VK_TARGET_GLSL_DS_EXPAND()

//
// If the host-side buffer structure is simply:
//
//   struct SPN_VK_TARGET_BUFFER_NAME(foo,bar) {
//     <type> bar[0];
//   };
//
// it will have a sizeof() equal to type (right?).
//
//

#define SPN_VK_TARGET_BUFFER_OFFSETOF(_ds_id, _name, _member)                                      \
  OFFSET_OF_MACRO(SPN_VK_TARGET_BUFFER_NAME(_ds_id, _name), _member)

//
// Define host-side pipeline push constant structures
//
//   struct spn_vk_push_block_pool_init
//   ...
//   struct spn_vk_push_render
//
//

#define SPN_VK_TARGET_PUSH_NAME(_p_id) struct spn_vk_push_##_p_id

#undef SPN_VK_TARGET_VK_DS
#define SPN_VK_TARGET_VK_DS(_p_id, _ds_idx, _ds_id)

#undef SPN_VK_TARGET_VK_PUSH
#define SPN_VK_TARGET_VK_PUSH(_p_id, _p_pc) SPN_VK_TARGET_PUSH_NAME(_p_id){ _p_pc };

#undef SPN_VK_TARGET_P_EXPAND_X
#define SPN_VK_TARGET_P_EXPAND_X(_p_idx, _p_id, _p_descs) _p_descs

SPN_VK_TARGET_P_EXPAND()

//
// Declare descriptor acquire/release/update functions
//

#define SPN_VK_TARGET_DS_TYPE(_ds_id) struct spn_vk_ds_##_ds_id##_t
#define SPN_VK_TARGET_DS_TYPE_DECLARE(_ds_id)                                                      \
  SPN_VK_TARGET_DS_TYPE(_ds_id)                                                                    \
  {                                                                                                \
    uint32_t idx;                                                                                  \
  }

#define SPN_VK_TARGET_DS_ACQUIRE_FUNC(_ds_id) spn_vk_ds_acquire_##_ds_id
#define SPN_VK_TARGET_DS_UPDATE_FUNC(_ds_id) spn_vk_ds_update_##_ds_id
#define SPN_VK_TARGET_DS_RELEASE_FUNC(_ds_id) spn_vk_ds_release_##_ds_id

#define SPN_VK_TARGET_DS_ACQUIRE_PROTO(_ds_id)                                                     \
  void SPN_VK_TARGET_DS_ACQUIRE_FUNC(_ds_id)(struct spn_vk * const                 instance,       \
                                             struct spn_device * const             device,         \
                                             SPN_VK_TARGET_DS_TYPE(_ds_id) * const ds)

#define SPN_VK_TARGET_DS_UPDATE_PROTO(_ds_id)                                                      \
  void SPN_VK_TARGET_DS_UPDATE_FUNC(_ds_id)(struct spn_vk * const             instance,            \
                                            struct spn_vk_environment * const environment,         \
                                            SPN_VK_TARGET_DS_TYPE(_ds_id) const ds)

#define SPN_VK_TARGET_DS_RELEASE_PROTO(_ds_id)                                                     \
  void SPN_VK_TARGET_DS_RELEASE_FUNC(_ds_id)(struct spn_vk * const instance,                       \
                                             SPN_VK_TARGET_DS_TYPE(_ds_id) const ds)

#undef SPN_VK_TARGET_DS_EXPAND_X
#define SPN_VK_TARGET_DS_EXPAND_X(_ds_idx, _ds_id, _ds)                                            \
  SPN_VK_TARGET_DS_TYPE_DECLARE(_ds_id);                                                           \
  SPN_VK_TARGET_DS_ACQUIRE_PROTO(_ds_id);                                                          \
  SPN_VK_TARGET_DS_UPDATE_PROTO(_ds_id);                                                           \
  SPN_VK_TARGET_DS_RELEASE_PROTO(_ds_id);

SPN_VK_TARGET_DS_EXPAND()

//
// Get references to descriptor set entries
//

#define SPN_VK_TARGET_DS_GET_FUNC(_ds_id, _d_id) spn_vk_ds_get_##_ds_id##_##_d_id

#define SPN_VK_TARGET_DS_GET_PROTO_STORAGE_BUFFER(_ds_id, _d_id)                                   \
  VkDescriptorBufferInfo * SPN_VK_TARGET_DS_GET_FUNC(_ds_id, _d_id)(                               \
    struct spn_vk * const instance,                                                                \
    SPN_VK_TARGET_DS_TYPE(_ds_id) const ds)

#define SPN_VK_TARGET_DS_GET_PROTO_STORAGE_IMAGE(_ds_id, _d_id)                                    \
  VkDescriptorImageInfo * SPN_VK_TARGET_DS_GET_FUNC(_ds_id, _d_id)(struct spn_vk * const instance, \
                                                                   SPN_VK_TARGET_DS_TYPE(_ds_id)   \
                                                                     const ds);

#undef SPN_VK_TARGET_DESC_TYPE_STORAGE_BUFFER
#define SPN_VK_TARGET_DESC_TYPE_STORAGE_BUFFER(_ds_id, _d_idx, _d_ext, _d_id)                      \
  SPN_VK_TARGET_DS_GET_PROTO_STORAGE_BUFFER(_ds_id, _d_id);

#undef SPN_VK_TARGET_DESC_TYPE_STORAGE_IMAGE
#define SPN_VK_TARGET_DESC_TYPE_STORAGE_IMAGE(_ds_id, _d_idx, _d_ext, _d_id)                       \
  SPN_VK_TARGET_DS_GET_PROTO_STORAGE_IMAGE(_ds_id, _d_id);

#undef SPN_VK_TARGET_DS_EXPAND_X
#define SPN_VK_TARGET_DS_EXPAND_X(_ds_idx, _ds_id, _ds) _ds

SPN_VK_TARGET_DS_EXPAND()

//
// Bind a pipeline-specific descriptor set to a command buffer
//

#define SPN_VK_TARGET_DS_BIND_FUNC(_p_id, _ds_id) spn_vk_ds_bind_##_p_id##_##_ds_id

#define SPN_VK_TARGET_DS_BIND_PROTO(_p_id, _ds_id)                                                 \
  void SPN_VK_TARGET_DS_BIND_FUNC(_p_id, _ds_id)(struct spn_vk * const instance,                   \
                                                 VkCommandBuffer       cb,                         \
                                                 SPN_VK_TARGET_DS_TYPE(_ds_id) const ds)

#undef SPN_VK_TARGET_VK_DS
#define SPN_VK_TARGET_VK_DS(_p_id, _ds_idx, _ds_id) SPN_VK_TARGET_DS_BIND_PROTO(_p_id, _ds_id);

#undef SPN_VK_TARGET_VK_PUSH
#define SPN_VK_TARGET_VK_PUSH(_p_id, _p_pc)

#undef SPN_VK_TARGET_P_EXPAND_X
#define SPN_VK_TARGET_P_EXPAND_X(_p_idx, _p_id, _p_descs) _p_descs

SPN_VK_TARGET_P_EXPAND()

//
// Write push constants to command buffer
//

#define SPN_VK_TARGET_P_PUSH_FUNC(_p_id) spn_vk_p_push_##_p_id

#define SPN_VK_TARGET_P_PUSH_PROTO(_p_id)                                                          \
  SPN_VK_TARGET_PUSH_NAME(_p_id);                                                                  \
  void SPN_VK_TARGET_P_PUSH_FUNC(_p_id)(struct spn_vk * const                        instance,     \
                                        VkCommandBuffer                              cb,           \
                                        SPN_VK_TARGET_PUSH_NAME(_p_id) const * const push)

#undef SPN_VK_TARGET_P_EXPAND_X
#define SPN_VK_TARGET_P_EXPAND_X(_p_idx, _p_id, _p_descs) SPN_VK_TARGET_P_PUSH_PROTO(_p_id);

SPN_VK_TARGET_P_EXPAND()

//
// Bind pipeline to command buffer
//

#define SPN_VK_TARGET_P_BIND_FUNC(_p_id) spn_vk_p_bind_##_p_id

#define SPN_VK_TARGET_P_BIND_PROTO(_p_id)                                                          \
  void SPN_VK_TARGET_P_BIND_FUNC(_p_id)(struct spn_vk * const instance, VkCommandBuffer cb)

#undef SPN_VK_TARGET_P_EXPAND_X
#define SPN_VK_TARGET_P_EXPAND_X(_p_idx, _p_id, _p_descs) SPN_VK_TARGET_P_BIND_PROTO(_p_id);

SPN_VK_TARGET_P_EXPAND()

//
//
//

#endif  // SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_SPN_VK_H_
