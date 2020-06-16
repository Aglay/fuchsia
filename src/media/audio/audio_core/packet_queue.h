// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_PACKET_QUEUE_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_PACKET_QUEUE_H_

#include <deque>
#include <memory>
#include <mutex>

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/media/audio/audio_core/clock_reference.h"
#include "src/media/audio/audio_core/packet.h"
#include "src/media/audio/audio_core/pending_flush_token.h"
#include "src/media/audio/audio_core/stream.h"
#include "src/media/audio/audio_core/versioned_timeline_function.h"
#include "src/media/audio/lib/format/format.h"

namespace media::audio {

class PacketQueue : public ReadableStream {
 public:
  PacketQueue(Format format, ClockReference ref_clock);
  PacketQueue(Format format,
              fbl::RefPtr<VersionedTimelineFunction> reference_clock_to_fractional_frames,
              ClockReference ref_clock);
  ~PacketQueue();

  bool empty() const {
    std::lock_guard<std::mutex> locker(pending_mutex_);
    return pending_packet_queue_.empty();
  }

  void set_usage(const StreamUsage& usage) {
    usage_mask_.clear();
    usage_mask_.insert(usage);
  }

  void PushPacket(const fbl::RefPtr<Packet>& packet);
  void Flush(const fbl::RefPtr<PendingFlushToken>& flush_token = nullptr);
  /// Report duration of underflow that occured
  void SetUnderflowReporter(fit::function<void(zx::duration)> underflow_reporter) {
    underflow_reporter_ = std::move(underflow_reporter);
  }

  // |media::audio::ReadableStream|
  std::optional<ReadableStream::Buffer> ReadLock(zx::time ref_time, int64_t frame,
                                                 uint32_t frame_count) override;
  void Trim(zx::time ref_time) override;
  TimelineFunctionSnapshot ReferenceClockToFractionalFrames() const override;
  void ReportUnderflow(FractionalFrames<int64_t> frac_source_start,
                       FractionalFrames<int64_t> frac_source_mix_point,
                       zx::duration underflow_duration) override;
  void ReportPartialUnderflow(FractionalFrames<int64_t> frac_source_offset,
                              int64_t dest_mix_offset) override;
  ClockReference reference_clock() const override { return reference_clock_; }

 private:
  void ReadUnlock(bool fully_consumed);

  StreamUsageMask usage_mask_;

  std::mutex flush_mutex_;
  mutable std::mutex pending_mutex_;

  std::deque<fbl::RefPtr<Packet>> pending_packet_queue_ FXL_GUARDED_BY(pending_mutex_);
  std::deque<fbl::RefPtr<Packet>> pending_flush_packet_queue_ FXL_GUARDED_BY(pending_mutex_);
  std::deque<fbl::RefPtr<PendingFlushToken>> pending_flush_token_queue_
      FXL_GUARDED_BY(pending_mutex_);
  bool flushed_ FXL_GUARDED_BY(pending_mutex_) = true;
  bool processing_in_progress_ FXL_GUARDED_BY(pending_mutex_) = false;
  fbl::RefPtr<VersionedTimelineFunction> timeline_function_;
  std::atomic<uint16_t> underflow_count_ = {0};
  std::atomic<uint16_t> partial_underflow_count_ = {0};
  fit::function<void(zx::duration)> underflow_reporter_;
  ClockReference reference_clock_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_PACKET_QUEUE_H_
