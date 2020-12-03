// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_EXAMPLES_VKPRIMER_COMMON_UTILS_H_
#define SRC_GRAPHICS_EXAMPLES_VKPRIMER_COMMON_UTILS_H_

#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#define RTN_MSG(err, ...)                          \
  {                                                \
    fprintf(stderr, "%s:%d ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__);                  \
    return err;                                    \
  }

// Log and return if |cond| is true.
#define RTN_IF_MSG(err, cond, ...)                 \
  if ((cond)) {                                    \
    fprintf(stderr, "%s:%d ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__);                  \
    return err;                                    \
  }

// Log and return based on VkResult |r|.
#define RTN_IF_VK_ERR(err, r, ...)                                      \
  if (r != VK_SUCCESS) {                                                \
    fprintf(stderr, "%s:%d:\n\t(vk::Result::e%s) ", __FILE__, __LINE__, \
            vk::to_string(vk::Result(r)).c_str());                      \
    fprintf(stderr, __VA_ARGS__);                                       \
    fprintf(stderr, "\n");                                              \
    fflush(stderr);                                                     \
    return err;                                                         \
  }

// Log and return based on vk::Result |r|.
#define RTN_IF_VKH_ERR(err, r, ...)                                                                \
  if (r != vk::Result::eSuccess) {                                                                 \
    fprintf(stderr, "%s:%d:\n\t(vk::Result::e%s) ", __FILE__, __LINE__, vk::to_string(r).c_str()); \
    fprintf(stderr, __VA_ARGS__);                                                                  \
    fprintf(stderr, "\n");                                                                         \
    fflush(stderr);                                                                                \
    return err;                                                                                    \
  }

namespace vkp {

enum SearchProp { INSTANCE_EXT_PROP, INSTANCE_LAYER_PROP, PHYS_DEVICE_EXT_PROP };

//
// Using the vkEnumerate* entrypoints, search for all elements of
// |required_props| to look for a match.  If all elements are found,
// return true.  If any are missing, return false and populate
// |missing_props_out| with the missing properties. If nullptr is
// passed for |missing_props_out|, it will be ignored.
//
// If |layer| is not nullptr, constrain the property search to the
// specified layer only.
//
// The type of enumeration entrypoint used is selected using the
// |search_prop| parameter.  Those 3 selectable entrypoints are:
//
//   vk::enumerateInstanceExtensionProperties
//   vk::enumerateInstanceLayerProperties
//   vk::enumerateDeviceExtensionProperties
//
bool FindRequiredProperties(const std::vector<const char *> &required_props, SearchProp search_prop,
                            vk::PhysicalDevice phys_device, const char *layer,
                            std::vector<std::string> *missing_props_out);

// Find graphics queue family index from physical device.  If |surface| is not nullptr,
// succeed only if queue family has present support.  If |queue_family_index| is not
// nullptr, store the queue family index into it if found.
bool FindGraphicsQueueFamilyIndex(vk::PhysicalDevice phys_device, VkSurfaceKHR surface = nullptr,
                                  uint32_t *queue_family_index = nullptr);

// Find physical device memory property index for |properties|.
int FindMemoryIndex(const vk::PhysicalDevice &phys_dev, uint32_t memory_type_bits,
                    const vk::MemoryPropertyFlags &memory_prop_flags);

// Log physical device memory properties.
void LogMemoryProperties(const vk::PhysicalDevice &phys_dev);

}  // namespace vkp

#endif  // SRC_GRAPHICS_EXAMPLES_VKPRIMER_COMMON_UTILS_H_
