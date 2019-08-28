// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_DEVICE_DEVICE_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_DEVICE_DEVICE_H_

/* Add an abstracted device interface that can be used for wlan driver tests without involving
 * devmgr.
 */

#include <list>

#include <ddk/device.h>
#include <ddk/driver.h>

namespace wlan {
namespace simulation {

#ifdef DEBUG
#define DBG_PRT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DBG_PRT(fmt, ...)
#endif // DEBUG

// Simulated device_add()
class FakeDevMgr {
 public:
  FakeDevMgr();
  ~FakeDevMgr();
  zx_status_t wlan_sim_device_add(zx_device_t* parent, device_add_args_t* args, zx_device_t** out);
  zx_status_t wlan_sim_device_remove(zx_device_t* device);
  zx_device_t* wlan_sim_device_get_first(zx_device_t** parent, device_add_args_t** args);
  zx_device_t* wlan_sim_device_get_next(zx_device_t** parent, device_add_args_t** args);
  size_t wlan_sim_device_get_num_devices();

 private:
  // device_list is of this type and not public
  using wlan_sim_dev_info_t =
  struct wlan_sim_dev_info {
      zx_device* parent;
      device_add_args_t dev_args;
  };

  std::list<wlan_sim_dev_info_t*> device_list_;
  std::list<wlan_sim_dev_info_t*>::iterator dev_list_itr_;
};
} // namespace simulation
}  // namespace wlan
#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_DEVICE_DEVICE_H_
