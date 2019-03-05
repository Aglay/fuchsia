// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_CONTROL_IMPL_H_
#define GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_CONTROL_IMPL_H_

#include <ddk/device.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/virtualaudio/c/fidl.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/zx/channel.h>

#include "garnet/drivers/audio/virtual_audio/virtual_audio_input_impl.h"
#include "garnet/drivers/audio/virtual_audio/virtual_audio_output_impl.h"

namespace virtual_audio {

class VirtualAudioDeviceImpl;

class VirtualAudioControlImpl : public fuchsia::virtualaudio::Control {
 public:
  VirtualAudioControlImpl() = default;

  // Always called after DdkRelease unless object is prematurely freed. This
  // would be a reference error: DevHost holds a reference until DdkRelease.
  ~VirtualAudioControlImpl() = default;

  // TODO(mpuryear): Move the three static methods and table over to DDKTL.
  //
  // Always called after DdkUnbind.
  static void DdkRelease(void* ctx);
  // Always called after our child drivers are unbound and released.
  static void DdkUnbind(void* ctx);
  // Delivers C-binding-FIDL Forwarder calls to the driver.
  static zx_status_t DdkMessage(void* ctx, fidl_msg_t* msg, fidl_txn_t* txn);

  //
  // virtualaudio.Forwarder interface
  //
  zx_status_t SendControl(zx::channel control_request_channel);
  zx_status_t SendInput(zx::channel input_request_channel);
  zx_status_t SendOutput(zx::channel output_request_channelf);

  //
  // virtualaudio.Control interface
  //
  void Enable() override;
  void Disable() override;

  void ReleaseBindings();
  bool enabled() const { return enabled_; }
  zx_device_t* dev_node() const { return dev_node_; }

 private:
  friend class VirtualAudioBus;
  friend class VirtualAudioInputImpl;
  friend class VirtualAudioOutputImpl;

  static fuchsia_virtualaudio_Forwarder_ops_t fidl_ops_;

  zx_device_t* dev_node_ = nullptr;
  bool enabled_ = true;

  fidl::BindingSet<fuchsia::virtualaudio::Control> bindings_;
  fidl::BindingSet<fuchsia::virtualaudio::Input,
                   fbl::unique_ptr<VirtualAudioInputImpl>>
      input_bindings_;
  fidl::BindingSet<fuchsia::virtualaudio::Output,
                   fbl::unique_ptr<VirtualAudioOutputImpl>>
      output_bindings_;
};

}  // namespace virtual_audio

#endif  // GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_CONTROL_IMPL_H_
