// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_BIN_CAMERA_GYM_STREAM_CYCLER_H_
#define SRC_CAMERA_BIN_CAMERA_GYM_STREAM_CYCLER_H_

#include <fuchsia/camera3/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fit/function.h>
#include <lib/fit/result.h>

namespace camera {

// This class is responsible for exercising the camera APIs to cycle between the various streams and
// configurations reported by a camera.
class StreamCycler {
 public:
  ~StreamCycler();
  static fit::result<std::unique_ptr<StreamCycler>, zx_status_t> Create(
      fuchsia::camera3::DeviceWatcherHandle watcher, fuchsia::sysmem::AllocatorHandle allocator);
  using AddCollectionHandler = fit::function<uint32_t(fuchsia::sysmem::BufferCollectionTokenHandle,
                                                      fuchsia::sysmem::ImageFormat_2)>;
  using RemoveCollectionHandler = fit::function<void(uint32_t)>;
  using ShowBufferHandler = fit::function<void(uint32_t, uint32_t, zx::eventpair)>;
  // Registers handlers that are called when the cycler adds or removes a buffer collection. The
  // value returned by |on_add_collection| will be subsequently passed to |on_remove_collection|.
  void SetHandlers(AddCollectionHandler on_add_collection,
                   RemoveCollectionHandler on_remove_collection, ShowBufferHandler on_show_buffer);

 private:
  StreamCycler();
  void WatchDevicesCallback(std::vector<fuchsia::camera3::WatchDevicesEvent> events);
  void ConnectToStream(uint32_t config_index, uint32_t stream_index);
  void OnNextFrame(uint32_t stream_index, fuchsia::camera3::FrameInfo frame_info);

  async::Loop loop_;
  fuchsia::camera3::DeviceWatcherPtr watcher_;
  fuchsia::sysmem::AllocatorPtr allocator_;
  fuchsia::camera3::DevicePtr device_;
  std::vector<fuchsia::camera3::Configuration> configurations_;
  AddCollectionHandler add_collection_handler_;
  RemoveCollectionHandler remove_collection_handler_;
  ShowBufferHandler show_buffer_handler_;

  // stream_infos_ uses the same index as the corresponding stream index in configurations_.
  struct StreamInfo {
    fuchsia::camera3::StreamPtr stream;
    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info;
    uint32_t add_collection_handler_returned_value;
  };
  std::map<uint32_t, StreamInfo> stream_infos_;
};

}  // namespace camera

#endif  // SRC_CAMERA_BIN_CAMERA_GYM_STREAM_CYCLER_H_
