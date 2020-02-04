// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_OUTPUT_PIPELINE_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_OUTPUT_PIPELINE_H_

#include <lib/media/cpp/timeline_function.h>

#include <vector>

#include <fbl/ref_ptr.h>
#include <trace/event.h>

#include "src/lib/syslog/cpp/logger.h"
#include "src/media/audio/audio_core/mix_stage.h"
#include "src/media/audio/audio_core/pipeline_config.h"
#include "src/media/audio/audio_core/stream.h"

namespace media::audio {

class OutputPipeline : public Stream {
 public:
  // Creates an |OutputPipeline| based on the specification in |config|. The pipeline will
  // ultimately produce output frames via |LockBuffer| in the |output_format| requested.
  //
  // |max_block_size_frames| is the largest contiguous region that may be returned from
  // |LockBuffer|. If a caller requests a frame region of more that |max_block_size_frames|, then
  // the pipeline will truncate this to only |max_block_size_frames| and the caller will have to
  // call |LockBuffer| again to mix the subsequent frames.
  //
  // |ref_clock_to_fractional_frame| is a timeline function that will compute the output frame
  // number (in fixed point format with 13 bits of fractional precision) based on a reference
  // timestamp.
  OutputPipeline(const PipelineConfig& config, const Format& output_format,
                 uint32_t max_block_size_frames,
                 TimelineFunction reference_clock_to_fractional_frame);

  // Returns the loopback |Stream| for this pipeline.
  std::shared_ptr<Stream> loopback() const { return loopback_; }

  // Adds |stream| as an input to be mixed. The given |usage| will indicate where in the pipeline
  // this stream will be routed (based on the |PipelineConfig| this pipeline was created with).
  std::shared_ptr<Mixer> AddInput(std::shared_ptr<Stream> stream,
                                  const fuchsia::media::Usage& usage);

  // Removes |stream| from the pipeline.
  //
  // It is an error to call |RemoveInput| without exactly one preceeding call to |AddInput| with the
  // same |stream|.
  void RemoveInput(const Stream& stream);

  // |media::audio::Stream|
  std::optional<Stream::Buffer> LockBuffer(zx::time ref_time, int64_t frame,
                                           uint32_t frame_count) override {
    TRACE_DURATION("audio", "OutputPipeline::LockBuffer");
    FX_DCHECK(stream_);
    return stream_->LockBuffer(ref_time, frame, frame_count);
  }
  void UnlockBuffer(bool release_buffer) override {
    TRACE_DURATION("audio", "OutputPipeline::UnlockBuffer");
    FX_DCHECK(stream_);
    stream_->UnlockBuffer(release_buffer);
  }
  void Trim(zx::time trim) override {
    TRACE_DURATION("audio", "OutputPipeline::Trim");
    FX_CHECK(stream_);
    stream_->Trim(trim);
  }
  TimelineFunctionSnapshot ReferenceClockToFractionalFrames() const override {
    TRACE_DURATION("audio", "OutputPipeline::ReferenceClockToFractionalFrames");
    FX_DCHECK(stream_);
    return stream_->ReferenceClockToFractionalFrames();
  }

 private:
  std::shared_ptr<Stream> CreateMixStage(
      const PipelineConfig::MixGroup& spec, const Format& output_format, uint32_t block_size,
      fbl::RefPtr<VersionedTimelineFunction> ref_clock_to_output_frame, uint32_t* usage_mask);
  MixStage& LookupStageForUsage(const fuchsia::media::Usage& usage);

  std::vector<std::pair<std::shared_ptr<MixStage>, std::vector<fuchsia::media::Usage>>> mix_stages_;
  std::vector<std::pair<std::shared_ptr<Stream>, fuchsia::media::Usage>> streams_;

  // This is the root of the mix graph. The other mix stages must be reachable from this node
  // to actually get mixed.
  std::shared_ptr<Stream> stream_;

  std::shared_ptr<Stream> loopback_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_OUTPUT_PIPELINE_H_
