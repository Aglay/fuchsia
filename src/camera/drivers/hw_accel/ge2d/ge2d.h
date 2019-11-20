// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_HW_ACCEL_GE2D_GE2D_H_
#define SRC_CAMERA_DRIVERS_HW_ACCEL_GE2D_GE2D_H_

#include <ddk/platform-defs.h>
#ifndef _ALL_SOURCE
#define _ALL_SOURCE  // Enables thrd_create_with_name in <threads.h>.
#include <threads.h>
#endif
#include <lib/device-protocol/pdev.h>
#include <lib/device-protocol/platform-device.h>
#include <lib/fidl-utils/bind.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/event.h>
#include <lib/zx/interrupt.h>
#include <zircon/fidl.h>

#include <atomic>
#include <deque>
#include <unordered_map>

#include <ddk/protocol/platform/bus.h>
#include <ddk/protocol/platform/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/amlogiccanvas.h>
#include <ddktl/protocol/composite.h>
#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>
#include <fbl/mutex.h>
#include <hw/reg.h>

#include "ge2d-task.h"

namespace ge2d {
// |Ge2dDevice| is spawned by the driver in |ge2d.cc|
namespace {

constexpr uint64_t kPortKeyIrqMsg = 0x00;
constexpr uint64_t kPortKeyDebugFakeInterrupt = 0x01;

}  // namespace

// This provides ZX_PROTOCOL_GE2D.
class Ge2dDevice;
using Ge2dDeviceType = ddk::Device<Ge2dDevice, ddk::UnbindableNew>;

class Ge2dDevice : public Ge2dDeviceType, public ddk::Ge2dProtocol<Ge2dDevice, ddk::base_protocol> {
 public:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Ge2dDevice);
  explicit Ge2dDevice(zx_device_t* parent, ddk ::MmioBuffer ge2d_mmio, zx::interrupt ge2d_irq,
                      zx::bti bti, zx::port port, amlogic_canvas_protocol_t canvas)
      : Ge2dDeviceType(parent),
        port_(std::move(port)),
        ge2d_mmio_(std::move(ge2d_mmio)),
        ge2d_irq_(std::move(ge2d_irq)),
        bti_(std::move(bti)),
        canvas_(canvas) {}

  // Setup() is used to create an instance of Ge2dDevice.
  static zx_status_t Setup(zx_device_t* parent, std::unique_ptr<Ge2dDevice>* out);

  // Methods required by the ddk.
  void DdkRelease();
  void DdkUnbindNew(ddk::UnbindTxn txn);

  // ZX_PROTOCOL_GE2DC (Refer to ge2d.banjo for documentation).
  zx_status_t Ge2dInitTaskResize(const buffer_collection_info_2_t* input_buffer_collection,
                                 const buffer_collection_info_2_t* output_buffer_collection,
                                 const resize_info_t* info,
                                 const image_format_2_t* input_image_format,
                                 const image_format_2_t* output_image_format_table_list,
                                 size_t output_image_format_table_count,
                                 uint32_t output_image_format_index,
                                 const hw_accel_frame_callback_t* frame_callback,
                                 const hw_accel_res_change_callback_t* res_callback,
                                 uint32_t* out_task_index);

  // See ge2d-task.h for description of args.
  zx_status_t Ge2dInitTaskWaterMark(const buffer_collection_info_2_t* input_buffer_collection,
                                    const buffer_collection_info_2_t* output_buffer_collection,
                                    const water_mark_info_t* info, zx::vmo watermark_vmo,
                                    const image_format_2_t* image_format_table_list,
                                    size_t image_format_table_count, uint32_t image_format_index,
                                    const hw_accel_frame_callback_t* frame_callback,
                                    const hw_accel_res_change_callback_t* res_callback,
                                    uint32_t* out_task_index);

  zx_status_t Ge2dProcessFrame(uint32_t task_index, uint32_t input_buffer_index);
  void Ge2dRemoveTask(uint32_t task_index);
  void Ge2dReleaseFrame(uint32_t task_index, uint32_t buffer_index);

  // Note that this is only supported on Watermark Tasks. The new format applies to both
  // input AND output formats.
  zx_status_t Ge2dSetInputAndOutputResolution(uint32_t task_index, uint32_t new_image_format_index);

  // Note that this is only supported on Resize Tasks.
  zx_status_t Ge2dSetOutputResolution(uint32_t task_index, uint32_t new_output_image_format_index);

  void Ge2dSetCropRectangle(uint32_t task_index, const crop_rectangle_t* crop);

  // Used for unit tests.
  const ddk::MmioBuffer* ge2d_mmio() const { return &ge2d_mmio_; }
  zx_status_t StartThread();
  zx_status_t StopThread();

 protected:
  enum Ge2dOp { GE2D_OP_SETOUTPUTRES, GE2D_OP_SETINPUTOUTPUTRES, GE2D_OP_FRAME };

  struct TaskInfo {
    Ge2dOp op;
    Ge2dTask* task;
    uint32_t index;
  };

  zx::port port_;

 private:
  friend class Ge2dDeviceTester;

  // All necessary clean up is done here in ShutDown().
  void ShutDown();
  int FrameProcessingThread();
  int JoinThread() { return thrd_join(processing_thread_, nullptr); }

  void ProcessTask(TaskInfo& info);
  zx_status_t WaitForInterrupt(zx_port_packet_t* packet);

  // Used to access the processing queue.
  fbl::Mutex lock_;

  ddk::MmioBuffer ge2d_mmio_;
  zx::interrupt ge2d_irq_;
  zx::bti bti_;
  uint32_t next_task_index_ = 0;
  std::unordered_map<uint32_t, std::unique_ptr<Ge2dTask>> task_map_;
  std::deque<TaskInfo> processing_queue_ __TA_GUARDED(lock_);
  thrd_t processing_thread_;
  fbl::ConditionVariable frame_processing_signal_ __TA_GUARDED(lock_);
  bool shutdown_ __TA_GUARDED(lock_) = false;
  amlogic_canvas_protocol_t canvas_ = {};
};
}  // namespace ge2d

#endif  // SRC_CAMERA_DRIVERS_HW_ACCEL_GE2D_GE2D_H_
