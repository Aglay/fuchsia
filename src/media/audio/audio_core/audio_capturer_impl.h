// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CAPTURER_IMPL_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CAPTURER_IMPL_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/media/cpp/timeline_function.h>
#include <lib/media/cpp/timeline_rate.h>

#include <atomic>
#include <memory>
#include <mutex>

#include <fbl/intrusive_double_list.h>
#include <fbl/slab_allocator.h>

#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/audio_object.h"
#include "src/media/audio/audio_core/link_matrix.h"
#include "src/media/audio/audio_core/mixer/mixer.h"
#include "src/media/audio/audio_core/mixer/output_producer.h"
#include "src/media/audio/audio_core/pending_capture_buffer.h"
#include "src/media/audio/audio_core/route_graph.h"
#include "src/media/audio/audio_core/stream_volume_manager.h"
#include "src/media/audio/audio_core/threading_model.h"
#include "src/media/audio/audio_core/usage_settings.h"
#include "src/media/audio/audio_core/utils.h"

namespace media::audio {

class AudioAdmin;
class AudioCoreImpl;

class AudioCapturerImpl : public AudioObject,
                          public fuchsia::media::AudioCapturer,
                          public fuchsia::media::audio::GainControl,
                          public StreamVolume {
 public:
  static std::unique_ptr<AudioCapturerImpl> Create(
      bool loopback, fidl::InterfaceRequest<fuchsia::media::AudioCapturer> audio_capturer_request,
      AudioCoreImpl* owner);
  static std::unique_ptr<AudioCapturerImpl> Create(
      bool loopback, fidl::InterfaceRequest<fuchsia::media::AudioCapturer> audio_capturer_request,
      ThreadingModel* threading_model, RouteGraph* route_graph, AudioAdmin* admin,
      StreamVolumeManager* volume_manager, LinkMatrix* link_matrix);

  ~AudioCapturerImpl() override;

 private:
  void OverflowOccurred(FractionalFrames<int64_t> source_start, FractionalFrames<int64_t> mix_point,
                        zx::duration overflow_duration);
  void PartialOverflowOccurred(FractionalFrames<int64_t> source_offset, int64_t mix_offset);

  // Notes about the AudioCapturerImpl state machine.
  // TODO(mpuryear): Update this comment block.
  //
  // :: WaitingForVmo ::
  // AudioCapturers start in this mode. They should have a default capture mode
  // set, and will accept a mode change up until the point where they have a
  // shared payload VMO assigned to them. At this point they transition into the
  // OperatingSync state. Only the main service thread may transition out of
  // this state.
  //
  // :: OperatingSync ::
  // After a mode has been assigned and a shared payload VMO has provided, the
  // AudioCapturer is now operating in synchronous mode. Clients may provided
  // buffers to be filled using the CaptureAt method and may cancel these
  // buffers using the Flush method. They may also transition to asynchronous
  // mode by calling StartAsyncCapture, but only when there are no pending
  // buffers in flight. Only the main service thread may transition out of
  // this state.
  //
  // :: OperatingAsync ::
  // AudioCapturers enter OperatingAsync after a successful call to
  // StartAsyncCapture. Threads from the mix_domain allocate and fill pending
  // payload buffers, then signal the main service thread in order to send them
  // back to the client over the AudioCapturerClient interface provided when
  // starting. CaptureAt and Flush are illegal operations while in this state.
  // clients may begin the process of returning to synchronous capture mode by
  // calling StopAsyncCapture. Only the main service thread may transition out
  // of this state.
  //
  // :: AsyncStopping ::
  // AudioCapturers enter AsyncStopping after a successful call to
  // StopAsyncCapture. A thread from the mix_domain will handle the details of
  // stopping, including transferring all partially filled pending buffers to
  // the finished queue. Aside from setting the gain, all operations are illegal
  // while the AudioCapturer is in the process of stopping. Once the mix domain
  // thread has finished cleaning up, it will transition to the
  // AsyncStoppingCallbackPending state and signal the main service thread in
  // order to complete the process. Only a mix domain thread may transition out
  // of this state.
  //
  // :: AsyncStoppingCallbackPending ::
  // AudioCapturers enter AsyncStoppingCallbackPending after a mix domain thread
  // has finished the process of shutting down the capture process and is ready
  // to signal to the client that the AudioCapturer is now in synchronous
  // capture mode again. The main service thread will send all partially and
  // completely filled buffers to the user, ensuring that there is at least one
  // buffer sent indicating end-of-stream, even if that buffer needs to be of
  // zero length. Finally, the main service thread will signal that the stopping
  // process is finished using the client supplied callback (if any), and
  // finally transition back to the OperatingSync state.
  enum class State {
    WaitingForVmo,
    OperatingSync,
    OperatingAsync,
    AsyncStopping,
    AsyncStoppingCallbackPending,
    Shutdown,
  };

  static bool StateIsRoutable(AudioCapturerImpl::State state) {
    return state != AudioCapturerImpl::State::WaitingForVmo &&
           state != AudioCapturerImpl::State::Shutdown;
  }

  AudioCapturerImpl(bool loopback,
                    fidl::InterfaceRequest<fuchsia::media::AudioCapturer> audio_capturer_request,
                    ThreadingModel* threading_model, RouteGraph* route_graph, AudioAdmin* admin,
                    StreamVolumeManager* volume_manager, LinkMatrix* link_matrix);

  using PcbList = ::fbl::DoublyLinkedList<std::unique_ptr<PendingCaptureBuffer>>;

  // |fuchsia::media::AudioCapturer|
  void GetStreamType(GetStreamTypeCallback cbk) final;
  void SetPcmStreamType(fuchsia::media::AudioStreamType stream_type) final;
  void AddPayloadBuffer(uint32_t id, zx::vmo payload_buf_vmo) final;
  void RemovePayloadBuffer(uint32_t id) final;
  void CaptureAt(uint32_t payload_buffer_id, uint32_t offset_frames, uint32_t num_frames,
                 CaptureAtCallback cbk) final;
  void ReleasePacket(fuchsia::media::StreamPacket packet) final;
  void DiscardAllPackets(DiscardAllPacketsCallback cbk) final;
  void DiscardAllPacketsNoReply() final;
  void StartAsyncCapture(uint32_t frames_per_packet) final;
  void StopAsyncCapture(StopAsyncCaptureCallback cbk) final;
  void StopAsyncCaptureNoReply() final;
  void BindGainControl(fidl::InterfaceRequest<fuchsia::media::audio::GainControl> request) final;
  void SetUsage(fuchsia::media::AudioCaptureUsage usage) final;

  // |fuchsia::media::audio::GainControl|
  void SetGain(float gain_db) final;
  void SetGainWithRamp(float gain_db, int64_t duration_ns,
                       fuchsia::media::audio::RampType ramp_type) final {
    FX_NOTIMPLEMENTED();
  }
  void SetMute(bool mute) final;
  void NotifyGainMuteChanged();

  void ReportStart();
  void ReportStop();

  // |media::audio::AudioObject|
  fit::result<std::shared_ptr<Mixer>, zx_status_t> InitializeSourceLink(
      const AudioObject& source, std::shared_ptr<Stream> stream) override;
  void CleanupSourceLink(const AudioObject& source, std::shared_ptr<Stream> stream) override;
  void OnLinkAdded() override;
  std::optional<fuchsia::media::Usage> usage() const override { return {UsageFrom(usage_)}; }

  // |media::audio::StreamVolume|
  bool GetStreamMute() const final;
  fuchsia::media::Usage GetStreamUsage() const final;
  void RealizeVolume(VolumeCommand volume_command) final;

  // Methods used by capture/mixer thread(s). Must be called from mix_domain.
  zx_status_t Process() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain_->token());
  bool MixToIntermediate(zx::time now, uint32_t mix_frames)
      FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain_->token());
  void UpdateTransformation(Mixer::Bookkeeping* bk, const AudioDriver::RingBufferSnapshot& rb_snap)
      FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain_->token());
  void DoStopAsyncCapture() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain_->token());
  bool QueueNextAsyncPendingBuffer() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain_->token())
      FXL_LOCKS_EXCLUDED(pending_lock_);
  void ShutdownFromMixDomain() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain_->token());

  // Thunk to send finished buffers back to the user, and to finish an async
  // mode stop operation.
  void FinishAsyncStopThunk() FXL_LOCKS_EXCLUDED(mix_domain_->token());
  void FinishBuffersThunk() FXL_LOCKS_EXCLUDED(mix_domain_->token());

  // Helper function used to return a set of pending capture buffers to a user.
  void FinishBuffers(const PcbList& finished_buffers) FXL_LOCKS_EXCLUDED(mix_domain_->token());

  // Mixer helper.
  void UpdateFormat(fuchsia::media::AudioStreamType stream_type)
      FXL_LOCKS_EXCLUDED(mix_domain_->token());

  // Select a mixer for the link supplied.
  fit::result<std::shared_ptr<Mixer>, zx_status_t> ChooseMixer(const AudioObject& source,
                                                               std::shared_ptr<Stream> stream);

  fit::promise<> Cleanup() FXL_LOCKS_EXCLUDED(mix_domain_->token());
  void CleanupFromMixThread() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain_->token());
  void MixTimerThunk() {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, mix_domain_);
    Process();
  }

  // Removes the capturer from its owner, the route graph, triggering shutdown and drop.
  void BeginShutdown();

  void SetRoutingProfile();

  void RecomputeMinFenceTime();

  void Shutdown(std::unique_ptr<AudioCapturerImpl> self)
      FXL_LOCKS_EXCLUDED(threading_model_.FidlDomain().token());

  fuchsia::media::AudioCaptureUsage usage_ = fuchsia::media::AudioCaptureUsage::FOREGROUND;
  fidl::Binding<fuchsia::media::AudioCapturer> binding_;
  fidl::BindingSet<fuchsia::media::audio::GainControl> gain_control_bindings_;
  ThreadingModel& threading_model_;
  ThreadingModel::OwnedDomainPtr mix_domain_;
  AudioAdmin& admin_;
  StreamVolumeManager& volume_manager_;
  RouteGraph& route_graph_;
  std::atomic<State> state_;
  const bool loopback_;
  zx::duration min_fence_time_;

  // Capture format and gain state.
  Format format_;
  TimelineRate dest_frames_to_clock_mono_rate_;
  uint32_t max_frames_per_capture_;
  std::atomic<float> stream_gain_db_;
  bool mute_;

  // Shared buffer state
  fzl::VmoMapper payload_buf_;
  uint32_t payload_buf_frames_ = 0;

  WakeupEvent mix_wakeup_;
  async::TaskClosureMethod<AudioCapturerImpl, &AudioCapturerImpl::MixTimerThunk> mix_timer_
      FXL_GUARDED_BY(mix_domain_->token()){this};

  // Queues of capture buffers from the client: waiting to be filled, or waiting to be returned.
  std::mutex pending_lock_;
  PcbList pending_capture_buffers_ FXL_GUARDED_BY(pending_lock_);
  PcbList finished_capture_buffers_ FXL_GUARDED_BY(pending_lock_);

  // Intermediate mixing buffer and output producer
  std::unique_ptr<OutputProducer> output_producer_;
  std::unique_ptr<float[]> mix_buf_;

  std::vector<LinkMatrix::LinkHandle> source_links_ FXL_GUARDED_BY(mix_domain_->token());

  // Capture bookkeeping
  bool async_mode_ = false;
  TimelineFunction dest_frames_to_clock_mono_ FXL_GUARDED_BY(mix_domain_->token());
  GenerationId dest_frames_to_clock_mono_gen_ FXL_GUARDED_BY(mix_domain_->token());
  int64_t frame_count_ FXL_GUARDED_BY(mix_domain_->token()) = 0;

  uint32_t async_frames_per_packet_;
  uint32_t async_next_frame_offset_ FXL_GUARDED_BY(mix_domain_->token()) = 0;
  StopAsyncCaptureCallback pending_async_stop_cbk_;

  // for glitch-debugging purposes
  std::atomic<uint16_t> overflow_count_;
  std::atomic<uint16_t> partial_overflow_count_;

  struct MixerHolder {
    // We hold a raw pointer to the |AudioObject| here since it is what we use to identify the mixer
    // between calls to |InitializeSourceLink| and |CleanupSourceLink|, and by freeing the
    // |MixerHolder| in |CleanupSourceLink|, we ensure we don't retain this pointer past the point
    // that the |AudioLink| is valid.
    //
    // Furthermore, we don't ever dereference this pointer, it's only used to identify the
    // corresponding |Mixer| in |CleanupSourceLink|.
    //
    // TODO(13688): This will be removed once we migrate AudioCapturerImpl to use the |MixStage|
    //              mix-pump logic.
    const AudioObject* object;
    std::shared_ptr<Mixer> mixer;
    MixerHolder(const AudioObject* _object, std::shared_ptr<Mixer> _mixer)
        : object(_object), mixer(std::move(_mixer)) {}
  };
  std::vector<MixerHolder> mixers_;

  LinkMatrix& link_matrix_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CAPTURER_IMPL_H_
