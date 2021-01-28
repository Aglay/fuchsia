// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_USB_AUDIO_USB_AUDIO_STREAM_H_
#define SRC_MEDIA_AUDIO_DRIVERS_USB_AUDIO_USB_AUDIO_STREAM_H_

#include <fuchsia/hardware/audio/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/wait.h>
#include <lib/zx/profile.h>
#include <lib/zx/vmo.h>
#include <zircon/listnode.h>

#include <memory>

#include <audio-proto/audio-proto.h>
#include <ddktl/device-internal.h>
#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <fbl/vector.h>
#include <usb/usb.h>

#include "debug-logging.h"

namespace audio {
namespace usb {

class UsbAudioDevice;
class UsbAudioStreamInterface;

struct AudioStreamProtocol : public ddk::internal::base_protocol {
  explicit AudioStreamProtocol(bool is_input) {
    ddk_proto_id_ = is_input ? ZX_PROTOCOL_AUDIO_INPUT : ZX_PROTOCOL_AUDIO_OUTPUT;
  }

  bool is_input() const { return (ddk_proto_id_ == ZX_PROTOCOL_AUDIO_INPUT); }
};

class UsbAudioStream;
using UsbAudioStreamBase = ddk::Device<UsbAudioStream, ddk::Messageable, ddk::Unbindable>;

class UsbAudioStream : public UsbAudioStreamBase,
                       public AudioStreamProtocol,
                       public fbl::RefCounted<UsbAudioStream>,
                       public fbl::DoublyLinkedListable<fbl::RefPtr<UsbAudioStream>>,
                       public ::llcpp::fuchsia::hardware::audio::Device::Interface {
 public:
  class Channel : public fbl::DoublyLinkedListable<fbl::RefPtr<Channel>>,
                  public fbl::RefCounted<Channel> {
   public:
    template <typename T = Channel, typename... ConstructorSignature>
    static fbl::RefPtr<T> Create(ConstructorSignature&&... args) {
      fbl::AllocChecker ac;
      auto ptr = fbl::AdoptRef(new (&ac) T(std::forward<ConstructorSignature>(args)...));

      if (!ac.check()) {
        return nullptr;
      }

      return ptr;
    }

    void SetHandler(async::Wait::Handler handler) { wait_.set_handler(std::move(handler)); }
    zx_status_t BeginWait(async_dispatcher_t* dispatcher) { return wait_.Begin(dispatcher); }
    zx_status_t Write(const void* buffer, uint32_t length) {
      return channel_.write(0, buffer, length, nullptr, 0);
    }
    zx_status_t Write(const void* buffer, uint32_t length, zx::handle&& handle) {
      zx_handle_t h = handle.release();
      return channel_.write(0, buffer, length, &h, 1);
    }
    zx_status_t Read(void* buffer, uint32_t length, uint32_t* out_length) {
      return channel_.read(0, buffer, nullptr, length, 0, out_length, nullptr);
    }

   protected:
    explicit Channel(zx::channel channel) : channel_(std::move(channel)) {
      wait_.set_object(channel_.get());
      wait_.set_trigger(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED);
    }
    ~Channel() = default;  // Deactivates (automatically cancels the wait from its RAII semantics).

   private:
    friend class fbl::RefPtr<Channel>;

    zx::channel channel_;
    async::Wait wait_;
  };
  static fbl::RefPtr<UsbAudioStream> Create(UsbAudioDevice* parent,
                                            std::unique_ptr<UsbAudioStreamInterface> ifc);
  zx_status_t Bind();
  void StreamChannelSignalled(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                              zx_status_t status, const zx_packet_signal_t* signal,
                              Channel* channel, bool priviledged);
  void RingBufferChannelSignalled(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                  zx_status_t status, const zx_packet_signal_t* signal,
                                  Channel* channel);

  const char* log_prefix() const { return log_prefix_; }

  // DDK device implementation
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();
  zx_status_t DdkMessage(fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
    DdkTransaction transaction(txn);
    llcpp::fuchsia::hardware::audio::Device::Dispatch(this, msg, &transaction);
    return transaction.Status();
  }

 private:
  friend class fbl::RefPtr<UsbAudioStream>;

  enum class RingBufferState {
    STOPPED,
    STOPPING,
    STOPPING_AFTER_UNPLUG,
    STARTING,
    STARTED,
  };

  UsbAudioStream(UsbAudioDevice* parent, std::unique_ptr<UsbAudioStreamInterface> ifc);
  virtual ~UsbAudioStream();

  void ComputePersistentUniqueId();

  void ReleaseRingBufferLocked() __TA_REQUIRES(lock_);

  // Device FIDL implementation
  void GetChannel(GetChannelCompleter::Sync& completer) override;

  // Thunks for dispatching stream channel events.
  zx_status_t ProcessStreamChannel(Channel* channel, bool privileged);
  void DeactivateStreamChannel(const Channel* channel);

  zx_status_t OnGetStreamFormatsLocked(Channel* channel, const audio_proto::StreamGetFmtsReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnSetStreamFormatLocked(Channel* channel, const audio_proto::StreamSetFmtReq& req,
                                      bool privileged) __TA_REQUIRES(lock_);
  zx_status_t OnGetGainLocked(Channel* channel, const audio_proto::GetGainReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnSetGainLocked(Channel* channel, const audio_proto::SetGainReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnPlugDetectLocked(Channel* channel, const audio_proto::PlugDetectReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnGetUniqueIdLocked(Channel* channel, const audio_proto::GetUniqueIdReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnGetStringLocked(Channel* channel, const audio_proto::GetStringReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnGetClockDomainLocked(Channel* channel, const audio_proto::GetClockDomainReq& req)
      __TA_REQUIRES(lock_);

  // Thunks for dispatching ring buffer channel events.
  zx_status_t ProcessRingBufferChannel(Channel* channel);
  void DeactivateRingBufferChannel(const Channel* channel);

  // Stream command handlers
  // Ring buffer command handlers
  zx_status_t OnGetFifoDepthLocked(Channel* channel, const audio_proto::RingBufGetFifoDepthReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnGetBufferLocked(Channel* channel, const audio_proto::RingBufGetBufferReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnStartLocked(Channel* channel, const audio_proto::RingBufStartReq& req)
      __TA_REQUIRES(lock_);
  zx_status_t OnStopLocked(Channel* channel, const audio_proto::RingBufStopReq& req)
      __TA_REQUIRES(lock_);

  void RequestComplete(usb_request_t* req);
  void QueueRequestLocked() __TA_REQUIRES(req_lock_);
  void CompleteRequestLocked(usb_request_t* req) __TA_REQUIRES(req_lock_);

  static void RequestCompleteCallback(void* ctx, usb_request_t* request);

  UsbAudioDevice& parent_;
  const std::unique_ptr<UsbAudioStreamInterface> ifc_;
  char log_prefix_[LOG_PREFIX_STORAGE] = {0};
  audio_stream_unique_id_t persistent_unique_id_;

  fbl::Mutex lock_;
  fbl::Mutex req_lock_ __TA_ACQUIRED_AFTER(lock_);

  // Dispatcher framework state
  fbl::RefPtr<Channel> stream_channel_ __TA_GUARDED(lock_);
  fbl::RefPtr<Channel> rb_channel_ __TA_GUARDED(lock_);

  int32_t clock_domain_;

  size_t selected_format_ndx_;
  uint32_t selected_frame_rate_;
  uint32_t frame_size_;
  uint32_t iso_packet_rate_;
  uint32_t bytes_per_packet_;
  uint32_t fifo_bytes_;
  uint32_t fractional_bpp_inc_;
  uint32_t fractional_bpp_acc_ __TA_GUARDED(req_lock_);
  uint32_t ring_buffer_offset_ __TA_GUARDED(req_lock_);
  uint64_t usb_frame_num_ __TA_GUARDED(req_lock_);

  uint32_t bytes_per_notification_ = 0;
  uint32_t notification_acc_ __TA_GUARDED(req_lock_);

  zx::vmo ring_buffer_vmo_;
  void* ring_buffer_virt_ = nullptr;
  uint32_t ring_buffer_size_ = 0;
  uint32_t ring_buffer_pos_ __TA_GUARDED(req_lock_);
  volatile RingBufferState ring_buffer_state_ __TA_GUARDED(req_lock_) = RingBufferState::STOPPED;

  union {
    audio_proto::RingBufStopResp stop;
    audio_proto::RingBufStartResp start;
  } pending_job_resp_ __TA_GUARDED(req_lock_);

  list_node_t free_req_ __TA_GUARDED(req_lock_);
  uint32_t free_req_cnt_ __TA_GUARDED(req_lock_);
  uint32_t allocated_req_cnt_;
  const zx_time_t create_time_;

  // TODO(johngro) : See MG-940.  eliminate this ASAP
  bool req_complete_prio_bumped_ = false;
  zx::profile profile_handle_;
  async::Loop loop_;
};

}  // namespace usb
}  // namespace audio

#endif  // SRC_MEDIA_AUDIO_DRIVERS_USB_AUDIO_USB_AUDIO_STREAM_H_
