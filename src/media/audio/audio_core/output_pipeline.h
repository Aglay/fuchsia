// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_OUTPUT_PIPELINE_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_OUTPUT_PIPELINE_H_

#include <lib/media/cpp/timeline_function.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>

#include <vector>

#include <fbl/ref_ptr.h>

#include "src/media/audio/audio_core/effects_stage.h"
#include "src/media/audio/audio_core/mix_stage.h"
#include "src/media/audio/audio_core/pipeline_config.h"
#include "src/media/audio/audio_core/stream.h"
#include "src/media/audio/audio_core/stream_usage.h"

namespace media::audio {

class OutputPipeline : public ReadableStream {
 public:
  explicit OutputPipeline(Format format) : ReadableStream(format) {}
  ~OutputPipeline() override = default;

  // Returns the loopback |ReadableStream| for this pipeline.
  virtual std::shared_ptr<ReadableStream> loopback() const = 0;

  // Adds |stream| as an input to be mixed. The given |usage| will indicate where in the pipeline
  // this stream will be routed.
  virtual std::shared_ptr<Mixer> AddInput(
      std::shared_ptr<ReadableStream> stream, const StreamUsage& usage,
      Mixer::Resampler sampler_hint = Mixer::Resampler::Default) = 0;

  // Removes |stream| from the pipeline.
  //
  // It is an error to call |RemoveInput| without exactly one preceeding call to |AddInput| with the
  // same |stream|.
  virtual void RemoveInput(const ReadableStream& stream) = 0;

  // Sets the configuration of all effects with the given instance name.
  virtual void SetEffectConfig(const std::string& instance_name, const std::string& config) = 0;
};

class OutputPipelineImpl : public OutputPipeline {
 public:
  // Creates an |OutputPipeline| based on the specification in |config|. The pipeline will
  // ultimately produce output frames via |ReadLock| in the |output_format| requested.
  //
  // |max_block_size_frames| is the largest contiguous region that may be returned from
  // |ReadLock|. If a caller requests a frame region of more that |max_block_size_frames|, then
  // the pipeline will truncate this to only |max_block_size_frames| and the caller will have to
  // call |ReadLock| again to mix the subsequent frames.
  //
  // |ref_clock_to_fractional_frame| is a timeline function that will compute the output frame
  // number (in fixed point format with 13 bits of fractional precision) based on a reference
  // timestamp.
  //
  // The |sampler| is optionally used to select the type of sampler to be used when joining
  // mix stages together.
  OutputPipelineImpl(const PipelineConfig& config, uint32_t channels,
                     uint32_t max_block_size_frames,
                     TimelineFunction reference_clock_to_fractional_frame,
                     Mixer::Resampler sampler = Mixer::Resampler::Default);
  ~OutputPipelineImpl() override = default;

  // |media::audio::OutputPipeline|
  std::shared_ptr<ReadableStream> loopback() const override { return loopback_; }
  std::shared_ptr<Mixer> AddInput(
      std::shared_ptr<ReadableStream> stream, const StreamUsage& usage,
      Mixer::Resampler sampler_hint = Mixer::Resampler::Default) override;
  void RemoveInput(const ReadableStream& stream) override;
  void SetEffectConfig(const std::string& instance_name, const std::string& config) override;

  // |media::audio::ReadableStream|
  std::optional<ReadableStream::Buffer> ReadLock(zx::time ref_time, int64_t frame,
                                                 uint32_t frame_count) override {
    TRACE_DURATION("audio", "OutputPipeline::ReadLock");
    FX_DCHECK(stream_);
    return stream_->ReadLock(ref_time, frame, frame_count);
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
  void SetMinLeadTime(zx::duration min_lead_time) override {
    ReadableStream::SetMinLeadTime(min_lead_time);
    stream_->SetMinLeadTime(min_lead_time);
  }

 private:
  std::shared_ptr<ReadableStream> CreateMixStage(
      const PipelineConfig::MixGroup& spec, uint32_t channels, uint32_t block_size,
      fbl::RefPtr<VersionedTimelineFunction> ref_clock_to_output_frame, uint32_t* usage_mask,
      Mixer::Resampler sampler);
  MixStage& LookupStageForUsage(const StreamUsage& usage);

  std::vector<std::pair<std::shared_ptr<MixStage>, std::vector<StreamUsage>>> mix_stages_;
  std::vector<std::shared_ptr<EffectsStage>> effects_stages_;
  std::vector<std::pair<std::shared_ptr<ReadableStream>, StreamUsage>> streams_;

  // This is the root of the mix graph. The other mix stages must be reachable from this node
  // to actually get mixed.
  std::shared_ptr<ReadableStream> stream_;

  std::shared_ptr<ReadableStream> loopback_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_OUTPUT_PIPELINE_H_
