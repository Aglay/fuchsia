// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_LIB_ESCHER_VK_VULKAN_INSTANCE_H_
#define SRC_UI_LIB_ESCHER_VK_VULKAN_INSTANCE_H_

#include <list>
#include <set>
#include <string>

#include "src/lib/fxl/memory/ref_counted.h"

#include <vulkan/vulkan.hpp>

namespace escher {

class VulkanInstance;
using VulkanInstancePtr = fxl::RefPtr<VulkanInstance>;

using VkDebugReportCallbackFn =
    std::function<VkBool32(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
                           uint64_t object, size_t location, int32_t messageCode,
                           const char* pLayerPrefix, const char* pMessage, void* pUserData)>;

// Convenient wrapper for creating and managing the lifecycle of a VkInstance
// that is suitable for use by Escher.
class VulkanInstance : public fxl::RefCountedThreadSafe<VulkanInstance> {
 public:
  // Parameters used to construct a new Vulkan Instance.
  struct Params {
    std::set<std::string> layer_names{"VK_LAYER_KHRONOS_validation"};
    std::set<std::string> extension_names;
    bool requires_surface = true;
  };

  // Contains dynamically-obtained addresses of instance-specific functions.
  struct ProcAddrs {
    ProcAddrs(vk::Instance instance, bool requires_surface);

    PFN_vkCreateDebugReportCallbackEXT vkCreateDebugReportCallbackEXT = nullptr;
    PFN_vkDestroyDebugReportCallbackEXT vkDestroyDebugReportCallbackEXT = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
  };

  // Contains debug report callback function and the user data pointer the
  // callback function binds to.
  struct DebugReportCallback {
    VkDebugReportCallbackFn function = nullptr;
    void* user_data = nullptr;

    DebugReportCallback(VkDebugReportCallbackFn function_, void* user_data_)
        : function(std::move(function_)), user_data(user_data_) {}
  };
  using DebugReportCallbackList = std::list<DebugReportCallback>;
  using DebugReportCallbackHandle = DebugReportCallbackList::iterator;

  // Constructor.
  static fxl::RefPtr<VulkanInstance> New(Params params);

  ~VulkanInstance();

  // Enumerate the available instance layers.  Return true if all required
  // layers are present, and false otherwise.
  static bool ValidateLayers(const std::set<std::string>& required_layer_names);

  // Enumerate the available instance extensions.  Return true if all required
  // extensions are present, and false otherwise.  NOTE: if an extension isn't
  // found at first, we look in all required layers to see if it is implemented
  // there.
  static bool ValidateExtensions(const std::set<std::string>& required_extension_names,
                                 const std::set<std::string>& required_layer_names);

  // Register debug report callback |function| to list of callbacks, which will
  // be invoked by |DebugReportCallbackEntrance| when validation error occurs.
  // The returned handle will be required when deregistering the callback.
  DebugReportCallbackHandle RegisterDebugReportCallback(VkDebugReportCallbackFn function,
                                                        void* user_data = nullptr);

  // Remove debug report callback pointed by |handle| from |callbacks_|, the list
  // of callback functions.
  void DeregisterDebugReportCallback(const DebugReportCallbackHandle& handle);

  vk::Instance vk_instance() const { return instance_; }

  // Return the parameterss that were used to create this instance.
  const Params& params() const { return params_; }

  // Return per-instance functions that were dynamically looked up.
  const ProcAddrs& proc_addrs() const { return proc_addrs_; }

 private:
  VulkanInstance(vk::Instance instance, Params params);

  // The "entrance" handler for all Vulkan instances. When validation error
  // occurs, this function will invoke all debug report callback functions
  // stored in vector |callbacks_|. This function always return VK_FALSE.
  static VkBool32 DebugReportCallbackEntrance(VkDebugReportFlagsEXT flags,
                                              VkDebugReportObjectTypeEXT objectType,
                                              uint64_t object, size_t location, int32_t messageCode,
                                              const char* pLayerPrefix, const char* pMessage,
                                              void* pUserData);

  bool HasDebugReportExt() const {
    return std::find(params_.extension_names.begin(), params_.extension_names.end(),
                     VK_EXT_DEBUG_REPORT_EXTENSION_NAME) != params_.extension_names.end();
  }

  vk::Instance instance_;
  Params params_;
  ProcAddrs proc_addrs_;
  std::list<DebugReportCallback> callbacks_;
  vk::DebugReportCallbackEXT vk_callback_entrance_handle_;
};

};  // namespace escher

#endif  // SRC_UI_LIB_ESCHER_VK_VULKAN_INSTANCE_H_
