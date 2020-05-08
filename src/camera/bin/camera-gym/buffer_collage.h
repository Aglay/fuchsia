// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_BIN_CAMERA_GYM_BUFFER_COLLAGE_H_
#define SRC_CAMERA_BIN_CAMERA_GYM_BUFFER_COLLAGE_H_

#include <fuchsia/math/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/promise.h>
#include <lib/fit/result.h>
#include <lib/ui/scenic/cpp/resources.h>

#include <map>

namespace camera {

struct CollectionView {
  fuchsia::sysmem::ImageFormat_2 image_format;
  fuchsia::sysmem::BufferCollectionPtr collection;
  fuchsia::sysmem::BufferCollectionInfo_2 buffers;
  fuchsia::images::ImagePipe2Ptr image_pipe;
  uint32_t image_pipe_id;
  std::unique_ptr<scenic::Material> material;
  std::unique_ptr<scenic::Rectangle> rectangle;
  std::unique_ptr<scenic::ShapeNode> node;
};

// This class takes ownership of the display and presents the contents of buffer collections in a
// grid pattern. Unless otherwise noted, public methods are thread-safe and private methods must
// only be called from the loop's thread.
class BufferCollage : public fuchsia::ui::app::ViewProvider,
                      public fuchsia::ui::policy::MediaButtonsListener {
 public:
  ~BufferCollage() override;

  // Creates a new BufferCollage instance using the provided interface handles. After returning, if
  // the instance stops running, either due to an error or explicit action, |stop_callback| is
  // invoked exactly once if non-null.
  static fit::result<std::unique_ptr<BufferCollage>, zx_status_t> Create(
      fuchsia::ui::scenic::ScenicHandle scenic, fuchsia::sysmem::AllocatorHandle allocator,
      fuchsia::ui::policy::DeviceListenerRegistryHandle registry,
      fit::closure stop_callback = nullptr);

  // Returns the view request handler.
  fidl::InterfaceRequestHandler<fuchsia::ui::app::ViewProvider> GetHandler();

  // Registers a new buffer collection and adds it to the view, updating the layout of existing
  // collections to fit. Returns an id representing the collection.
  fit::promise<uint32_t> AddCollection(fuchsia::sysmem::BufferCollectionTokenHandle token,
                                       fuchsia::sysmem::ImageFormat_2 image_format,
                                       std::string description);

  // Removes the collection with the given |id| from the view and updates the layout to fill the
  // vacated space. If |id| is not a valid collection, the instance stops.
  void RemoveCollection(uint32_t id);

  // Updates the view to show the given |buffer_index| in for the given |collection_id|'s node.
  // Holds |release_fence| until the buffer is no longer needed, then closes the handle. If
  // non-null, |subregion| specifies what sub-region of the buffer to display.
  void PostShowBuffer(uint32_t collection_id, uint32_t buffer_index, zx::eventpair release_fence,
                      std::optional<fuchsia::math::Rect> subregion);

 private:
  BufferCollage();

  // Requests a new view.
  void OnNewRequest(fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider> request);

  // Disconnects all channels, quits the loop, and calls the stop callback.
  void Stop();

  // Registers the provider interface's error handler to invoke Stop.
  template <typename T>
  void SetStopOnError(fidl::InterfacePtr<T>& p, std::string name = T::Name_);

  // See PostShowBuffer.
  void ShowBuffer(uint32_t collection_id, uint32_t buffer_index, zx::eventpair release_fence,
                  std::optional<fuchsia::math::Rect> subregion);

  // Repositions scenic nodes to fit all collections on the screen.
  void UpdateLayout();

  // |scenic::Session| callbacks.
  void OnScenicError(zx_status_t status);
  void OnScenicEvent(std::vector<fuchsia::ui::scenic::Event> events);

  // |fuchsia::ui::app::ViewProvider|
  void CreateView(zx::eventpair view_token,
                  fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services,
                  fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> outgoing_services) override;

  // |fuchsia::ui::policy::MediaButtonsListener|
  void OnMediaButtonsEvent(fuchsia::ui::input::MediaButtonsEvent event) override;

  async::Loop loop_;
  fuchsia::ui::scenic::ScenicPtr scenic_;
  fuchsia::sysmem::AllocatorPtr allocator_;
  fuchsia::ui::policy::DeviceListenerRegistryPtr registry_;
  fidl::Binding<fuchsia::ui::policy::MediaButtonsListener> button_listener_binding_;
  fit::closure stop_callback_;
  std::unique_ptr<scenic::Session> session_;
  std::unique_ptr<scenic::View> view_;
  fidl::Binding<fuchsia::ui::app::ViewProvider> view_provider_binding_;
  std::optional<fuchsia::ui::gfx::BoundingBox> view_extents_;
  std::map<uint32_t, CollectionView> collection_views_;
  bool camera_muted_;
  uint32_t next_collection_id_ = 1;
};

}  // namespace camera

#endif  // SRC_CAMERA_BIN_CAMERA_GYM_BUFFER_COLLAGE_H_
