// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/bin/camera-gym/buffer_collage.h"

#include <fuchsia/images/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/dispatcher.h>
#include <lib/fit/bridge.h>
#include <lib/fit/promise.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/ui/scenic/cpp/resources.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>
#include <lib/zx/eventpair.h>
#include <zircon/errors.h>
#include <zircon/syscalls/object.h>
#include <zircon/types.h>

namespace camera {

// Returns an event such that when the event is signaled and the dispatcher executed, the provided
// eventpair is closed. This can be used to bridge event- and eventpair-based fence semantics. If
// this function returns an error, |eventpair| is closed immediately.
fit::result<zx::event, zx_status_t> MakeEventBridge(async_dispatcher_t* dispatcher,
                                                    zx::eventpair eventpair) {
  zx::event caller_event;
  zx::event waiter_event;
  zx_status_t status = zx::event::create(0, &caller_event);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fit::error(status);
  }
  status = caller_event.duplicate(ZX_RIGHT_SAME_RIGHTS, &waiter_event);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fit::error(status);
  }
  // A shared_ptr is necessary in order to begin the wait after setting the wait handler.
  auto wait = std::make_shared<async::Wait>(waiter_event.get(), ZX_EVENT_SIGNALED);
  wait->set_handler(
      [wait, waiter_event = std::move(waiter_event), eventpair = std::move(eventpair)](
          async_dispatcher_t* /*unused*/, async::Wait* /*unused*/, zx_status_t /*unused*/,
          const zx_packet_signal_t* /*unused*/) mutable {
        // Close the waiter along with its captures.
        wait = nullptr;
      });
  status = wait->Begin(dispatcher);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fit::error(status);
  }
  return fit::ok(std::move(caller_event));
}

BufferCollage::BufferCollage()
    : loop_(&kAsyncLoopConfigNoAttachToCurrentThread),
      button_listener_binding_(this),
      view_provider_binding_(this) {
  SetStopOnError(scenic_);
  SetStopOnError(allocator_);
  view_provider_binding_.set_error_handler([this](zx_status_t status) {
    FX_PLOGS(DEBUG, status) << "ViewProvider client disconnected.";
    view_provider_binding_.Unbind();
  });
}

BufferCollage::~BufferCollage() {
  zx_status_t status =
      async::PostTask(loop_.dispatcher(), fit::bind_member(this, &BufferCollage::Stop));
  ZX_ASSERT(status == ZX_OK);
  loop_.JoinThreads();
}

fit::result<std::unique_ptr<BufferCollage>, zx_status_t> BufferCollage::Create(
    fuchsia::ui::scenic::ScenicHandle scenic, fuchsia::sysmem::AllocatorHandle allocator,
    fuchsia::ui::policy::DeviceListenerRegistryHandle registry, fit::closure stop_callback) {
  auto collage = std::unique_ptr<BufferCollage>(new BufferCollage);

  // Bind interface handles and save the stop callback.
  zx_status_t status = collage->scenic_.Bind(std::move(scenic), collage->loop_.dispatcher());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fit::error(status);
  }
  status = collage->allocator_.Bind(std::move(allocator), collage->loop_.dispatcher());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fit::error(status);
  }
  status = collage->registry_.Bind(std::move(registry), collage->loop_.dispatcher());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fit::error(status);
  }
  collage->stop_callback_ = std::move(stop_callback);

  // Create a scenic session and set its event handlers.
  collage->session_ =
      std::make_unique<scenic::Session>(collage->scenic_.get(), collage->loop_.dispatcher());
  collage->session_->set_error_handler(
      fit::bind_member(collage.get(), &BufferCollage::OnScenicError));
  collage->session_->set_event_handler(
      fit::bind_member(collage.get(), &BufferCollage::OnScenicEvent));

  // Register the class as a button listener.
  collage->registry_->RegisterMediaButtonsListener(
      collage->button_listener_binding_.NewBinding(collage->loop_.dispatcher()));

  // Start a thread and begin processing messages.
  status = collage->loop_.StartThread("BufferCollage Loop");
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fit::error(status);
  }

  return fit::ok(std::move(collage));
}

fidl::InterfaceRequestHandler<fuchsia::ui::app::ViewProvider> BufferCollage::GetHandler() {
  return fit::bind_member(this, &BufferCollage::OnNewRequest);
}

fit::promise<uint32_t> BufferCollage::AddCollection(
    fuchsia::sysmem::BufferCollectionTokenHandle token, fuchsia::sysmem::ImageFormat_2 image_format,
    std::string description) {
  auto collection_id = next_collection_id_++;
  FX_LOGS(DEBUG) << "Adding collection with ID " << collection_id << ".";
  ZX_ASSERT(collection_views_.find(collection_id) == collection_views_.end());
  auto& collection_view = collection_views_[collection_id];
  std::ostringstream oss;
  oss << " (" << collection_id << ")";
  SetStopOnError(collection_view.collection, "Collection" + oss.str());
  SetStopOnError(collection_view.image_pipe, "Image Pipe" + oss.str());
  collection_view.image_format = image_format;

  // Bind and duplicate the token.
  fuchsia::sysmem::BufferCollectionTokenPtr token_ptr;
  SetStopOnError(token_ptr);
  zx_status_t status = token_ptr.Bind(std::move(token), loop_.dispatcher());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    Stop();
    return fit::make_result_promise<uint32_t>(fit::error());
  }
  fuchsia::sysmem::BufferCollectionTokenHandle scenic_token;
  token_ptr->Duplicate(ZX_RIGHT_SAME_RIGHTS, scenic_token.NewRequest());
  allocator_->BindSharedCollection(std::move(token_ptr),
                                   collection_view.collection.NewRequest(loop_.dispatcher()));

  // Sync the collection and create an image pipe using the scenic token.
  fit::bridge scenic_bridge;
  collection_view.collection->Sync([this, collection_id, token = std::move(scenic_token),
                                    result = std::move(scenic_bridge.completer)]() mutable {
    auto& view = collection_views_[collection_id];
    view.image_pipe_id = session_->AllocResourceId();
    auto command = scenic::NewCreateImagePipe2Cmd(view.image_pipe_id,
                                                  view.image_pipe.NewRequest(loop_.dispatcher()));
    session_->Enqueue(std::move(command));
    view.image_pipe->AddBufferCollection(1, std::move(token));
    UpdateLayout();
    result.complete_ok();
  });

  // Set minimal constraints then wait for buffer allocation.
  collection_view.collection->SetConstraints(true, {.usage{.none = fuchsia::sysmem::noneUsage}});
  fit::bridge sysmem_bridge;
  collection_view.collection->WaitForBuffersAllocated(
      [this, collection_id, result = std::move(sysmem_bridge.completer)](
          zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) mutable {
        if (status != ZX_OK) {
          FX_PLOGS(ERROR, status) << "Failed to allocate buffers.";
          Stop();
          result.complete_error();
          return;
        }
        collection_views_[collection_id].buffers = std::move(buffers);
        result.complete_ok();
      });

  // Once both scenic and sysmem complete their operations, add the negotiated images to the image
  // pipe. Note that this continuation may be run on an arbitrary thread, so private actions must be
  // marshalled back to the collage thread.
  return fit::join_promises(scenic_bridge.consumer.promise(), sysmem_bridge.consumer.promise())
      .then([this, collection_id,
             image_format](fit::result<std::tuple<fit::result<>, fit::result<>>>& result)
                -> fit::promise<uint32_t> {
        if (result.is_error() || std::get<0>(result.value()).is_error() ||
            std::get<1>(result.value()).is_error()) {
          FX_LOGS(ERROR) << "Failed to add collection " << collection_id << ".";
          zx_status_t status = async::PostTask(loop_.dispatcher(), [this] { Stop(); });
          if (status != ZX_OK) {
            FX_PLOGS(ERROR, status) << "Failed to schedule task.";
          }
          return fit::make_result_promise<uint32_t>(fit::error());
        }

        fit::bridge<uint32_t> task_bridge;
        zx_status_t status = async::PostTask(
            loop_.dispatcher(), [this, collection_id, image_format,
                                 result = std::move(task_bridge.completer)]() mutable {
              auto& view = collection_views_[collection_id];
              for (uint32_t i = 0; i < view.buffers.buffer_count; ++i) {
                view.image_pipe->AddImage(i + 1, 1, i, image_format);
              }
              FX_LOGS(DEBUG) << "Successfully added collection " << collection_id << ".";
              result.complete_ok(collection_id);
            });
        if (status != ZX_OK) {
          FX_PLOGS(ERROR, status) << "Failed to schedule task.";
          return fit::make_result_promise<uint32_t>(fit::error());
        }

        return task_bridge.consumer.promise();
      });
}

void BufferCollage::RemoveCollection(uint32_t id) {
  async::PostTask(loop_.dispatcher(), [this, id]() {
    auto it = collection_views_.find(id);
    if (it == collection_views_.end()) {
      FX_LOGS(ERROR) << "Invalid collection ID " << id << ".";
      Stop();
      return;
    }
    auto& collection_view = it->second;
    auto image_pipe_id = collection_view.image_pipe_id;
    view_->DetachChild(*collection_view.node);
    session_->ReleaseResource(image_pipe_id);  // De-allocate ImagePipe2 scenic side
    collection_view.collection->Close();
    collection_views_.erase(it);
    UpdateLayout();
  });
}

void BufferCollage::PostShowBuffer(uint32_t collection_id, uint32_t buffer_index,
                                   zx::eventpair release_fence,
                                   std::optional<fuchsia::math::Rect> subregion) {
  async::PostTask(loop_.dispatcher(), [=, release_fence = std::move(release_fence)]() mutable {
    ShowBuffer(collection_id, buffer_index, std::move(release_fence), subregion);
  });
}

void BufferCollage::OnNewRequest(fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider> request) {
  if (view_provider_binding_.is_bound()) {
    FX_LOGS(ERROR) << "Camera Gym only supports one view provider instance.";
    request.Close(ZX_ERR_NOT_SUPPORTED);
    return;
  }

  view_provider_binding_.Bind(std::move(request), loop_.dispatcher());
}

void BufferCollage::Stop() {
  if (view_provider_binding_.is_bound()) {
    FX_LOGS(WARNING) << "Collage closing view channel due to server error.";
    view_provider_binding_.Close(ZX_ERR_INTERNAL);
  }
  scenic_ = nullptr;
  allocator_ = nullptr;
  registry_ = nullptr;
  view_ = nullptr;
  collection_views_.clear();
  loop_.Quit();
  if (stop_callback_) {
    stop_callback_();
    stop_callback_ = nullptr;
  }
}

template <typename T>
void BufferCollage::SetStopOnError(fidl::InterfacePtr<T>& p, std::string name) {
  p.set_error_handler([this, name, &p](zx_status_t status) {
    FX_PLOGS(ERROR, status) << name << " disconnected unexpectedly.";
    p = nullptr;
    Stop();
  });
}

void BufferCollage::ShowBuffer(uint32_t collection_id, uint32_t buffer_index,
                               zx::eventpair release_fence,
                               std::optional<fuchsia::math::Rect> subregion) {
  if (subregion) {
    FX_LOGS(ERROR) << "Subregion is not yet supported.";
    Stop();
    return;
  }
  auto it = collection_views_.find(collection_id);
  if (it == collection_views_.end()) {
    FX_LOGS(ERROR) << "Invalid collection ID " << collection_id << ".";
    Stop();
    return;
  }
  if (buffer_index >= it->second.buffers.buffer_count) {
    FX_LOGS(ERROR) << "Invalid buffer index " << buffer_index << ".";
    Stop();
    return;
  }

  auto result = MakeEventBridge(loop_.dispatcher(), std::move(release_fence));
  if (result.is_error()) {
    FX_PLOGS(ERROR, result.error());
    Stop();
    return;
  }
  std::vector<zx::event> scenic_fences;
  scenic_fences.push_back(result.take_value());
  it->second.image_pipe->PresentImage(buffer_index + 1, zx::clock::get_monotonic().get(), {},
                                      std::move(scenic_fences),
                                      [](fuchsia::images::PresentationInfo info) {});
}

// Calculate the grid size needed to fit |n| elements by alternately adding rows and columns.
static std::tuple<uint32_t, uint32_t> GetGridSize(uint32_t n) {
  uint32_t rows = 0;
  uint32_t cols = 0;
  while (rows * cols < n) {
    if (rows == cols) {
      ++cols;
    } else {
      ++rows;
    }
  }
  return {rows, cols};
}

// Calculate the center of an element |index| in a grid with |n| elements.
static std::tuple<float, float> GetCenter(uint32_t index, uint32_t n) {
  auto [rows, cols] = GetGridSize(n);
  uint32_t row = index / cols;
  uint32_t col = index % cols;
  float y = (row + 0.5f) / rows;
  float x = (col + 0.5f) / cols;
  // Center-align the last row if it is not fully filled.
  if (row == rows - 1) {
    x += static_cast<float>(rows * cols - n) * 0.5f / cols;
  }
  return {x, y};
}

// Calculate the size of an element scaled uniformly to fit a given extent.
static std::tuple<float, float> ScaleToFit(float element_width, float element_height,
                                           float box_width, float box_height) {
  float x_scale = box_width / element_width;
  float y_scale = box_height / element_height;
  float scale = std::min(x_scale, y_scale);
  return {element_width * scale, element_height * scale};
}

void BufferCollage::UpdateLayout() {
  // TODO(49070): resolve constraints even if node is not visible
  // There is no intrinsic need to present the views prior to extents being known.
  if (!view_extents_) {
    constexpr fuchsia::ui::gfx::BoundingBox kDefaultBoundingBox{
        .min{.x = 0, .y = 0, .z = 0}, .max{.x = 640, .y = 480, .z = 1024}};
    view_extents_ = kDefaultBoundingBox;
  }

  auto [rows, cols] = GetGridSize(collection_views_.size());
  float view_width = view_extents_->max.x - view_extents_->min.x;
  float view_height = view_extents_->max.y - view_extents_->min.y;
  constexpr float kPadding = 4.0f;
  float cell_width = view_width / cols - kPadding;
  float cell_height = view_height / rows - kPadding;

  for (auto& [id, view] : collection_views_) {
    if (view.node) {
      view_->DetachChild(*view.node);
    }
  }
  uint32_t index = 0;
  for (auto& [id, view] : collection_views_) {
    view.material = std::make_unique<scenic::Material>(session_.get());
    view.material->SetTexture(view.image_pipe_id);
    if (camera_muted_) {
      view.material->SetColor(0, 0, 0, 0);
    }
    auto [element_width, element_height] = ScaleToFit(
        view.image_format.coded_width, view.image_format.coded_height, cell_width, cell_height);
    view.rectangle =
        std::make_unique<scenic::Rectangle>(session_.get(), element_width, element_height);
    view.node = std::make_unique<scenic::ShapeNode>(session_.get());
    view.node->SetShape(*view.rectangle);
    view.node->SetMaterial(*view.material);
    auto [x, y] = GetCenter(index++, collection_views_.size());
    view.node->SetTranslation(view_width * x, view_height * y, 0);
    // TODO(msandy): Track hidden nodes.
    if (view_) {
      view_->AddChild(*view.node);
    }
  }
  session_->Present(zx::clock::get_monotonic(), [](fuchsia::images::PresentationInfo info) {});
}

void BufferCollage::OnScenicError(zx_status_t status) {
  FX_PLOGS(ERROR, status) << "Scenic session error.";
  Stop();
}

void BufferCollage::OnScenicEvent(std::vector<fuchsia::ui::scenic::Event> events) {
  for (const auto& event : events) {
    if (event.is_gfx() && event.gfx().is_view_properties_changed()) {
      auto aabb = event.gfx().view_properties_changed().properties.bounding_box;
      // TODO(49069): bounding box should never be empty
      if (aabb.max.x == aabb.min.x || aabb.max.y == aabb.min.y || aabb.max.z == aabb.min.z) {
        view_extents_.reset();
      } else {
        view_extents_ = aabb;
      }
      UpdateLayout();
    }
  }
}

void BufferCollage::OnMediaButtonsEvent(fuchsia::ui::input::MediaButtonsEvent event) {
  if (event.has_mic_mute()) {
    camera_muted_ = event.mic_mute();
    FX_LOGS(INFO) << "Mic and Camera are " << (camera_muted_ ? "muted" : "unmuted") << ".";
    UpdateLayout();
  }
}

void BufferCollage::CreateView(
    zx::eventpair view_token,
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services,
    fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> outgoing_services) {
  if (view_) {
    FX_LOGS(ERROR) << "Clients may only call this method once per view provider lifetime.";
    view_provider_binding_.Close(ZX_ERR_BAD_STATE);
    Stop();
    return;
  }
  view_ = std::make_unique<scenic::View>(session_.get(), scenic::ToViewToken(std::move(view_token)),
                                         "Camera Gym");
  UpdateLayout();
}

}  // namespace camera
