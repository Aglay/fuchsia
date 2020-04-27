// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/bin/device/stream_impl.h"

#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/fit/function.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/zx/time.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "src/camera/bin/device/messages.h"
#include "src/camera/bin/device/util.h"
#include "src/lib/syslog/cpp/logger.h"

static fuchsia::math::Size ConvertToSize(fuchsia::sysmem::ImageFormat_2 format) {
  ZX_DEBUG_ASSERT(format.coded_width < std::numeric_limits<int32_t>::max());
  ZX_DEBUG_ASSERT(format.coded_height < std::numeric_limits<int32_t>::max());
  return {.width = static_cast<int32_t>(format.coded_width),
          .height = static_cast<int32_t>(format.coded_height)};
}

StreamImpl::StreamImpl(const fuchsia::camera3::StreamProperties& properties,
                       const fuchsia::camera2::hal::StreamConfig& legacy_config,
                       fidl::InterfaceRequest<fuchsia::camera3::Stream> request,
                       StreamRequestedCallback on_stream_requested, fit::closure on_no_clients)
    : loop_(&kAsyncLoopConfigNoAttachToCurrentThread),
      properties_(properties),
      legacy_config_(legacy_config),
      on_stream_requested_(std::move(on_stream_requested)),
      on_no_clients_(std::move(on_no_clients)) {
  legacy_stream_.set_error_handler(fit::bind_member(this, &StreamImpl::OnLegacyStreamDisconnected));
  legacy_stream_.events().OnFrameAvailable = fit::bind_member(this, &StreamImpl::OnFrameAvailable);
  current_resolution_ = ConvertToSize(properties.image_format);
  OnNewRequest(std::move(request));
  ZX_ASSERT(loop_.StartThread("Camera Stream Thread") == ZX_OK);
}

StreamImpl::~StreamImpl() {
  Unbind(legacy_stream_);
  async::PostTask(loop_.dispatcher(), [this] {
    for (auto& it : frame_waiters_) {
      // TODO(50018): async::Wait destructor ordering edge case
      it.second->Cancel();
      it.second = nullptr;
    }
    loop_.Quit();
  });
  loop_.JoinThreads();
}

void StreamImpl::OnNewRequest(fidl::InterfaceRequest<fuchsia::camera3::Stream> request) {
  zx_status_t status =
      async::PostTask(loop_.dispatcher(), [this, request = std::move(request)]() mutable {
        auto client = std::make_unique<Client>(*this, client_id_next_, std::move(request));
        client->PostReceiveResolution(current_resolution_);
        clients_.emplace(client_id_next_++, std::move(client));
      });
  ZX_ASSERT(status == ZX_OK);
}

void StreamImpl::OnLegacyStreamDisconnected(zx_status_t status) {
  FX_PLOGS(ERROR, status) << "Legacy Stream disconnected unexpectedly.";
  clients_.clear();
  on_no_clients_();
}

void StreamImpl::PostRemoveClient(uint64_t id) {
  async::PostTask(loop_.dispatcher(), [=]() {
    clients_.erase(id);
    if (clients_.empty()) {
      on_no_clients_();
    }
  });
}

void StreamImpl::PostAddFrameSink(uint64_t id) {
  async::PostTask(loop_.dispatcher(), [=]() {
    frame_sinks_.push(id);
    SendFrames();
  });
}

void StreamImpl::OnFrameAvailable(fuchsia::camera2::FrameAvailableInfo info) {
  if (info.frame_status != fuchsia::camera2::FrameStatus::OK) {
    FX_LOGS(WARNING) << "Driver reported a bad frame. This will not be reported to clients.";
    legacy_stream_->AcknowledgeFrameError();
    return;
  }

  if (!info.metadata.has_timestamp()) {
    FX_LOGS(WARNING)
        << "Driver sent a frame without a timestamp. This frame will not be sent to clients.";
    return;
  }

  // Construct the frame info and create the release fence.
  fuchsia::camera3::FrameInfo frame;
  frame.buffer_index = info.buffer_id;
  frame.frame_counter = ++frame_counter_;
  frame.timestamp = info.metadata.timestamp();
  zx::eventpair fence;
  ZX_ASSERT(zx::eventpair::create(0u, &fence, &frame.release_fence) == ZX_OK);
  frames_.push(std::move(frame));

  // Release frames in excess of the camping limit.
  while (frames_.size() > max_camping_buffers_) {
    frames_.pop();
  }

  // Queue a waiter so that when the client end of the fence is released, the frame is released back
  // to the driver.
  auto waiter =
      std::make_unique<async::Wait>(fence.get(), ZX_EVENTPAIR_PEER_CLOSED, 0,
                                    [this, fence = std::move(fence), index = frame.buffer_index](
                                        async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                        zx_status_t status, const zx_packet_signal_t* signal) {
                                      legacy_stream_->ReleaseFrame(index);
                                      frame_waiters_.erase(index);
                                    });
  ZX_ASSERT(waiter->Begin(loop_.dispatcher()) == ZX_OK);
  frame_waiters_[frame.buffer_index] = std::move(waiter);

  // Send the frame to any pending recipients.
  SendFrames();
}

void StreamImpl::PostSetBufferCollection(
    uint64_t id, fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) {
  async::PostTask(loop_.dispatcher(), [this, id, token_handle = std::move(token)]() mutable {
    auto it = clients_.find(id);
    if (it == clients_.end()) {
      FX_LOGS(ERROR) << "Client " << id << " not found.";
      token_handle.BindSync()->Close();
      ZX_DEBUG_ASSERT(false);
      return;
    }
    auto& client = it->second;

    // If null, just unregister the client and return.
    if (!token_handle) {
      client->Participant() = false;
      return;
    }

    client->Participant() = true;

    // Bind and duplicate the token for each participating client.
    fuchsia::sysmem::BufferCollectionTokenPtr token;
    ZX_ASSERT(token.Bind(std::move(token_handle), loop_.dispatcher()) == ZX_OK);
    for (auto& client : clients_) {
      if (client.second->Participant()) {
        fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> client_token;
        token->Duplicate(ZX_RIGHT_SAME_RIGHTS, client_token.NewRequest());
        client.second->PostReceiveBufferCollection(std::move(client_token));
      }
    }

    // Synchronize duplications then pass the final token to the device for constraints application.
    token->Sync([this, token = std::move(token)]() mutable {
      ZX_ASSERT(on_stream_requested_);
      frame_waiters_.clear();
      on_stream_requested_(
          std::move(token), legacy_stream_.NewRequest(loop_.dispatcher()),
          [this](uint32_t max_camping_buffers) { max_camping_buffers_ = max_camping_buffers; },
          legacy_stream_format_index_);
      legacy_stream_->Start();
    });
  });
}

void StreamImpl::SendFrames() {
  if (frame_sinks_.size() > 1 && !frame_sink_warning_sent_) {
    FX_LOGS(INFO) << Messages::kMultipleFrameClients;
    frame_sink_warning_sent_ = true;
  }

  while (!frames_.empty() && !frame_sinks_.empty()) {
    auto it = clients_.find(frame_sinks_.front());
    frame_sinks_.pop();
    if (it != clients_.end()) {
      it->second->PostSendFrame(std::move(frames_.front()));
      frames_.pop();
    }
  }
}

void StreamImpl::PostSetResolution(uint32_t id, fuchsia::math::Size coded_size) {
  zx_status_t status = async::PostTask(loop_.dispatcher(), [this, id, coded_size] {
    auto it = clients_.find(id);
    if (it == clients_.end()) {
      FX_LOGS(ERROR) << "Client " << id << " not found.";
      ZX_DEBUG_ASSERT(false);
      return;
    }
    auto& client = it->second;

    // Begin with the full resolution.
    auto best_size = ConvertToSize(properties_.image_format);
    if (coded_size.width > best_size.width || coded_size.height > best_size.height) {
      client->PostCloseConnection(ZX_ERR_INVALID_ARGS);
      return;
    }

    // Examine all supported resolutions, preferring those that cover the requested resolution but
    // have fewer pixels, breaking ties by picking the one with a smaller width.
    uint32_t best_index = 0;
    for (uint32_t i = 0; i < legacy_config_.image_formats.size(); ++i) {
      auto size = ConvertToSize(legacy_config_.image_formats[i]);
      bool contains_request = size.width >= coded_size.width && size.height >= coded_size.height;
      bool smaller_size = size.width * size.height < best_size.width * best_size.height;
      bool equal_size = size.width * size.height == best_size.width * best_size.height;
      bool smaller_width = size.width < best_size.width;
      if (contains_request && (smaller_size || (equal_size && smaller_width))) {
        best_size = size;
        best_index = i;
      }
    }

    // Save the selected image format, and set it on the stream if bound.
    legacy_stream_format_index_ = best_index;
    if (legacy_stream_) {
      legacy_stream_->SetImageFormat(legacy_stream_format_index_, [this](zx_status_t status) {
        if (status != ZX_OK) {
          FX_PLOGS(ERROR, status) << "Unexpected response from driver.";
          while (!clients_.empty()) {
            auto it = clients_.begin();
            it->second->PostCloseConnection(ZX_ERR_INTERNAL);
            clients_.erase(it);
          }
          on_no_clients_();
          return;
        }
      });
    }
    current_resolution_ = best_size;

    // Inform clients of the resolution change.
    for (auto& [id, client] : clients_) {
      client->PostReceiveResolution(best_size);
    }
  });
  ZX_DEBUG_ASSERT(status == ZX_OK);
}
