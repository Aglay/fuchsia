// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/mediaplayer/fidl/fidl_decoder.h"
#include <vector>
#include "garnet/bin/mediaplayer/fidl/fidl_type_conversions.h"
#include "garnet/bin/mediaplayer/graph/formatting.h"
#include "garnet/bin/mediaplayer/graph/types/audio_stream_type.h"
#include "lib/fidl/cpp/clone.h"
#include "lib/fidl/cpp/optional.h"

namespace media_player {
namespace {

static const char kAacAdtsMimeType[] = "audio/aac-adts";

// Creates codec_oob_bytes from a packet payload of at least 4 bytes.
std::vector<uint8_t> MakeOobBytesFromAdtsHeader(const uint8_t* adts_header) {
  std::vector<uint8_t> asc(2);

  // TODO(dustingreen): Switch from ADTS to .mp4 and fix AAC decoder to not
  // require "AudioSpecificConfig()" when fed ADTS.  In other words, move the
  // stuff here into a shim around the AAC OMX decoder, just next to (above or
  // below) the OmxCodecRunner in the codec_runner_sw_omx isolate, probably.

  // For SoftAAC2.cpp, for no particularly good reason, a CODECCONFIG buffer is
  // expected, even when running in ADTS mode, despite all the relevant data
  // being available from the ADTS header.  The CODECCONFIG buffer has an
  // AudioSpecificConfig in it.  The AudioSpecificConfig has to be created based
  // on corresponding fields of the ADTS header - not that requiring this of
  // the codec client makes any sense whatsoever...
  //
  // TODO(dustingreen): maybe add a per-codec compensation layer to un-crazy the
  // quirks of each codec.  For example, when decoding ADTS, all the needed info
  // is there in the ADTS stream directly.  No reason to hassle the codec client
  // for a pointless translated form of the same info.  In contrast, when it's
  // an mp4 file (or mkv, or whatever modern container format), the codec config
  // info is relevant.  But we should only force a client to provide it if
  // it's really needed.

  uint8_t profile_ObjectType;        // name in AAC spec in adts_fixed_header
  uint8_t sampling_frequency_index;  // name in AAC spec in adts_fixed_header
  uint8_t channel_configuration;     // name in AAC spec in adts_fixed_header
  profile_ObjectType = (adts_header[2] >> 6) & 0x3;
  sampling_frequency_index = (adts_header[2] >> 2) & 0xf;
  FXL_DCHECK(sampling_frequency_index < 11);
  channel_configuration = (adts_header[2] & 0x1) << 2 | (adts_header[3] >> 6);

  // Now let's convert these to the forms needed by AudioSpecificConfig.
  uint8_t audioObjectType =
      profile_ObjectType + 1;  // see near Table 1.A.11, for AAC not MPEG-2
  uint8_t samplingFrequencyIndex =
      sampling_frequency_index;                          // no conversion needed
  uint8_t channelConfiguration = channel_configuration;  // no conversion needed
  uint8_t frameLengthFlag = 0;
  uint8_t dependsOnCoreCoder = 0;
  uint8_t extensionFlag = 0;

  // Now we are ready to build a two-byte AudioSpecificConfig.  Not an
  // AudioSpecificInfo as stated in avc_utils.cpp (AOSP) mind you, but an
  // AudioSpecificConfig.
  asc[0] = (audioObjectType << 3) | (samplingFrequencyIndex >> 1);
  asc[1] = (samplingFrequencyIndex << 7) | (channelConfiguration << 3) |
           (frameLengthFlag << 2) | (dependsOnCoreCoder << 1) |
           (extensionFlag << 0);

  return asc;
}

}  // namespace

// static
void FidlDecoder::Create(
    const StreamType& stream_type,
    fuchsia::mediacodec::CodecFormatDetails input_format_details,
    fuchsia::mediacodec::CodecPtr decoder,
    fit::function<void(std::shared_ptr<Decoder>)> callback) {
  auto fidl_decoder = std::make_shared<FidlDecoder>(
      stream_type, std::move(input_format_details));
  fidl_decoder->Init(
      std::move(decoder),
      [fidl_decoder, callback = std::move(callback)](bool succeeded) {
        callback(succeeded ? fidl_decoder : nullptr);
      });
}

FidlDecoder::FidlDecoder(
    const StreamType& stream_type,
    fuchsia::mediacodec::CodecFormatDetails input_format_details)
    : medium_(stream_type.medium()),
      input_format_details_(std::move(input_format_details)) {
  update_oob_bytes_ = (input_format_details_.mime_type == kAacAdtsMimeType);

  switch (medium_) {
    case StreamType::Medium::kAudio:
      output_stream_type_ =
          AudioStreamType::Create(StreamType::kAudioEncodingLpcm, nullptr,
                                  AudioStreamType::SampleFormat::kNone, 1, 1);
      break;
    case StreamType::Medium::kVideo:
      output_stream_type_ = VideoStreamType::Create(
          StreamType::kVideoEncodingUncompressed, nullptr,
          VideoStreamType::VideoProfile::kUnknown,
          VideoStreamType::PixelFormat::kUnknown,
          VideoStreamType::ColorSpace::kUnknown, 0, 0, 0, 0, 1, 1,
          std::vector<uint32_t>(), std::vector<uint32_t>());
      break;
    case StreamType::Medium::kText:
    case StreamType::Medium::kSubpicture:
      FXL_CHECK(false) << "Only audio and video are supported.";
      break;
  }
}

void FidlDecoder::Init(fuchsia::mediacodec::CodecPtr decoder,
                       fit::function<void(bool)> callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(decoder);
  FXL_DCHECK(callback);

  outboard_decoder_ = std::move(decoder);
  init_callback_ = std::move(callback);

  outboard_decoder_.set_error_handler(
      fit::bind_member(this, &FidlDecoder::OnConnectionFailed));

  outboard_decoder_.events().OnStreamFailed =
      fit::bind_member(this, &FidlDecoder::OnStreamFailed);
  outboard_decoder_.events().OnInputConstraints =
      fit::bind_member(this, &FidlDecoder::OnInputConstraints);
  outboard_decoder_.events().OnOutputConfig =
      fit::bind_member(this, &FidlDecoder::OnOutputConfig);
  outboard_decoder_.events().OnOutputPacket =
      fit::bind_member(this, &FidlDecoder::OnOutputPacket);
  outboard_decoder_.events().OnOutputEndOfStream =
      fit::bind_member(this, &FidlDecoder::OnOutputEndOfStream);
  outboard_decoder_.events().OnFreeInputPacket =
      fit::bind_member(this, &FidlDecoder::OnFreeInputPacket);

  outboard_decoder_->EnableOnStreamFailed();
}

FidlDecoder::~FidlDecoder() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
}

const char* FidlDecoder::label() const { return "fidl decoder"; }

void FidlDecoder::Dump(std::ostream& os) const {
  GenericNode::Dump(os);
  // TODO(dalesat): More.
}

void FidlDecoder::ConfigureConnectors() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  MaybeConfigureInput(nullptr);
  MaybeConfigureOutput(nullptr);
}

void FidlDecoder::OnInputConnectionReady(size_t input_index) {
  FXL_DCHECK(input_index == 0);

  if (add_input_buffers_pending_) {
    add_input_buffers_pending_ = false;
    AddInputBuffers();
  }
}

void FidlDecoder::FlushInput(bool hold_frame, size_t input_index,
                             fit::closure callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(input_index == 0);
  FXL_DCHECK(callback);

  // This decoder will always receive a FlushOutput shortly after a FlushInput.
  // We call CloseCurrentStream now to let the outboard decoder know we're
  // abandoning this stream. Incrementing stream_lifetime_ordinal_ will cause
  // any stale output packets to be discarded. When FlushOutput is called, we'll
  // sync with the outboard decoder to make sure we're all caught up.
  outboard_decoder_->CloseCurrentStream(stream_lifetime_ordinal_, false, false);
  stream_lifetime_ordinal_ += 2;
  end_of_input_stream_ = false;
  update_oob_bytes_ = (input_format_details_.mime_type == kAacAdtsMimeType);
  flushing_ = true;

  callback();
}

void FidlDecoder::PutInputPacket(PacketPtr packet, size_t input_index) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(packet);
  FXL_DCHECK(input_index == 0);
  FXL_DCHECK(input_buffers_.has_current_set());

  if (flushing_) {
    return;
  }

  if (pts_rate_ == media::TimelineRate()) {
    pts_rate_ = packet->pts_rate();
  } else {
    FXL_DCHECK(pts_rate_ == packet->pts_rate());
  }

  if (packet->size() != 0) {
    // The buffer attached to this packet will be one we created using
    // |input_buffers_|.

    BufferSet& current_set = input_buffers_.current_set();

    // TODO(dalesat): Remove when the aac/adts decoder no longer needs this
    // help.
    if (update_oob_bytes_ && packet->size() >= 4) {
      FXL_DCHECK(packet->payload());

      input_format_details_.codec_oob_bytes =
          fidl::VectorPtr<uint8_t>(MakeOobBytesFromAdtsHeader(
              static_cast<const uint8_t*>(packet->payload())));

      outboard_decoder_->QueueInputFormatDetails(
          stream_lifetime_ordinal_, fidl::Clone(input_format_details_));
      update_oob_bytes_ = false;
    }

    FXL_DCHECK(packet->payload_buffer()->id() < current_set.buffer_count())
        << "Buffer ID " << packet->payload_buffer()->id()
        << " is out of range, should be less than "
        << current_set.buffer_count();
    current_set.AddRefBufferForDecoder(packet->payload_buffer()->id(),
                                       packet->payload_buffer());

    fuchsia::mediacodec::CodecPacket codec_packet;
    codec_packet.header.buffer_lifetime_ordinal =
        current_set.lifetime_ordinal();
    codec_packet.header.packet_index = packet->payload_buffer()->id();
    codec_packet.buffer_index = packet->payload_buffer()->id();
    codec_packet.stream_lifetime_ordinal = stream_lifetime_ordinal_;
    codec_packet.start_offset = 0;
    codec_packet.valid_length_bytes = packet->size();
    codec_packet.timestamp_ish = static_cast<uint64_t>(packet->pts());
    codec_packet.has_timestamp_ish = true;
    codec_packet.start_access_unit = packet->keyframe();
    codec_packet.known_end_access_unit = false;

    FXL_DCHECK(packet->size() <= current_set.buffer_size());

    outboard_decoder_->QueueInputPacket(std::move(codec_packet));
  }

  if (packet->end_of_stream()) {
    end_of_input_stream_ = true;
    outboard_decoder_->QueueInputEndOfStream(stream_lifetime_ordinal_);
  }
}

void FidlDecoder::OnOutputConnectionReady(size_t output_index) {
  FXL_DCHECK(output_index == 0);

  if (add_output_buffers_pending_) {
    add_output_buffers_pending_ = false;
    AddOutputBuffers();
  }
}

void FidlDecoder::FlushOutput(size_t output_index, fit::closure callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(output_index == 0);
  FXL_DCHECK(callback);

  // This decoder will always receive a FlushInput shortly before a FlushOutput.
  // In FlushInput, we've already closed the stream. Now we sync with the
  // output decoder just to make sure we're caught up.
  outboard_decoder_->Sync(std::move(callback));
}

void FidlDecoder::RequestOutputPacket() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  flushing_ = false;

  MaybeRequestInputPacket();
}

std::unique_ptr<StreamType> FidlDecoder::output_stream_type() const {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(output_stream_type_);
  return output_stream_type_->Clone();
}

void FidlDecoder::InitSucceeded() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (init_callback_) {
    auto callback = std::move(init_callback_);
    callback(true);
  }
}

void FidlDecoder::InitFailed() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (init_callback_) {
    auto callback = std::move(init_callback_);
    callback(false);
  }
}

void FidlDecoder::MaybeConfigureInput(
    fuchsia::mediacodec::CodecBufferConstraints* constraints) {
  if (stage() == nullptr) {
    // The node isn't ready to configure.
    if (constraints != nullptr) {
      cached_input_constraints_ = std::move(*constraints);
      input_constraints_cached_ = true;
    }

    return;
  }

  // The node is ready.
  if (constraints == nullptr) {
    if (!input_constraints_cached_) {
      // We have no constraints to apply. Defer the configuration.
      stage()->ConfigureInputDeferred();
      return;
    }

    constraints = &cached_input_constraints_;
    input_constraints_cached_ = false;
  }

  FXL_DCHECK(input_buffers_.has_current_set());

  BufferSet& current_set = input_buffers_.current_set();
  stage()->ConfigureInputToUseVmos(
      0, current_set.buffer_count(), current_set.buffer_size(),
      current_set.single_vmo() ? VmoAllocation::kSingleVmo
                               : VmoAllocation::kVmoPerBuffer,
      constraints->is_physically_contiguous_required,
      std::move(constraints->very_temp_kludge_bti_handle),
      [this, &current_set](uint64_t size, const PayloadVmos& payload_vmos) {
        // This callback runs on an arbitrary thread.
        return current_set.AllocateBuffer(size, payload_vmos);
      });

  if (stage()->InputConnectionReady()) {
    AddInputBuffers();
  } else {
    add_input_buffers_pending_ = true;
  }
}

void FidlDecoder::AddInputBuffers() {
  FXL_DCHECK(stage());
  FXL_DCHECK(stage()->InputConnectionReady());

  BufferSet& current_set = input_buffers_.current_set();
  for (uint32_t index = 0; index < current_set.buffer_count(); ++index) {
    auto descriptor =
        current_set.GetBufferDescriptor(index, false, stage()->UseInputVmos());
    outboard_decoder_->AddInputBuffer(std::move(descriptor));
  }
}

void FidlDecoder::MaybeConfigureOutput(
    fuchsia::mediacodec::CodecBufferConstraints* constraints) {
  FXL_DCHECK(constraints == nullptr ||
             constraints->per_packet_buffer_bytes_max != 0);

  if (stage() == nullptr) {
    // The node isn't ready to configure.
    if (constraints != nullptr) {
      cached_output_constraints_ = std::move(*constraints);
      output_constraints_cached_ = true;
    }

    return;
  }

  // The node is ready.
  if (constraints == nullptr) {
    if (!output_constraints_cached_) {
      // We have no constraints to apply. Defer the configuration.
      stage()->ConfigureOutputDeferred();
      return;
    }

    constraints = &cached_output_constraints_;
    output_constraints_cached_ = false;
  }

  FXL_DCHECK(output_buffers_.has_current_set());
  FXL_DCHECK(output_stream_type_);

  // TODO(dalesat): Do we need to add some buffers for queueing?
  BufferSet& current_set = output_buffers_.current_set();
  output_vmos_physically_contiguous_ =
      constraints->is_physically_contiguous_required;
  stage()->ConfigureOutputToUseVmos(
      0, current_set.buffer_count(), current_set.buffer_size(),
      current_set.single_vmo() ? VmoAllocation::kSingleVmo
                               : VmoAllocation::kVmoPerBuffer,
      output_vmos_physically_contiguous_,
      std::move(constraints->very_temp_kludge_bti_handle));

  if (stage()->OutputConnectionReady()) {
    AddOutputBuffers();
  } else {
    add_output_buffers_pending_ = true;
  }
}

void FidlDecoder::AddOutputBuffers() {
  FXL_DCHECK(stage());
  FXL_DCHECK(stage()->OutputConnectionReady());

  BufferSet& current_set = output_buffers_.current_set();
  current_set.AllocateAllBuffersForDecoder(stage()->UseOutputVmos());

  for (uint32_t index = 0; index < current_set.buffer_count(); ++index) {
    auto descriptor =
        current_set.GetBufferDescriptor(index, true, stage()->UseOutputVmos());
    outboard_decoder_->AddOutputBuffer(std::move(descriptor));
  }
}

void FidlDecoder::MaybeRequestInputPacket() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (!flushing_ && input_buffers_.has_current_set() && !end_of_input_stream_) {
    // |HasFreeBuffer| returns true if there's a free buffer. If there's no
    // free buffer, it will call the callback when there is one.
    if (!input_buffers_.current_set().HasFreeBuffer(
            [this]() { PostTask([this]() { MaybeRequestInputPacket(); }); })) {
      return;
    }

    if (!have_real_output_stream_type_) {
      if (pre_stream_type_packet_requests_remaining_ != 0) {
        --pre_stream_type_packet_requests_remaining_;
      } else {
        return;
      }
    }

    stage()->RequestInputPacket();
  }
}

void FidlDecoder::OnConnectionFailed() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  InitFailed();
  // TODO(dalesat): Report failure.
}

void FidlDecoder::OnStreamFailed(uint64_t stream_lifetime_ordinal) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  // TODO(dalesat): Report failure.
}

void FidlDecoder::OnInputConstraints(
    fuchsia::mediacodec::CodecBufferConstraints constraints) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(!input_buffers_.has_current_set())
      << "OnInputConstraints received more than once.";

  input_buffers_.ApplyConstraints(constraints, true);
  FXL_DCHECK(input_buffers_.has_current_set());
  BufferSet& current_set = input_buffers_.current_set();

  MaybeConfigureInput(&constraints);

  outboard_decoder_->SetInputBufferSettings(current_set.settings());

  InitSucceeded();
}

void FidlDecoder::OnOutputConfig(
    fuchsia::mediacodec::CodecOutputConfig config) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  auto stream_type =
      fxl::To<std::unique_ptr<StreamType>>(config.format_details);
  if (!stream_type) {
    FXL_LOG(ERROR) << "Can't comprehend format details.";
    InitFailed();
    return;
  }

  if (output_stream_type_) {
    if (output_format_details_version_ordinal_ !=
        config.format_details.format_details_version_ordinal) {
      HandlePossibleOutputStreamTypeChange(*output_stream_type_, *stream_type);
    }
  }

  output_format_details_version_ordinal_ =
      config.format_details.format_details_version_ordinal;

  output_stream_type_ = std::move(stream_type);
  have_real_output_stream_type_ = true;

  if (!config.buffer_constraints_action_required) {
    if (init_callback_) {
      FXL_LOG(ERROR)
          << "OnOutputConfig: action not required on initial config.";
      InitFailed();
    }

    return;
  }

  if (output_buffers_.has_current_set()) {
    output_buffers_.current_set().ReleaseAllDecoderOwnedBuffers();
  }

  // Use a single VMO for audio, VMO per buffer for video.
  output_buffers_.ApplyConstraints(
      config.buffer_constraints,
      output_stream_type_->medium() == StreamType::Medium::kAudio);

  FXL_DCHECK(output_buffers_.has_current_set());
  BufferSet& current_set = output_buffers_.current_set();

  outboard_decoder_->SetOutputBufferSettings(current_set.settings());

  // Create the VMOs when we're ready, and add them to the outboard decoder.
  MaybeConfigureOutput(&config.buffer_constraints);
};

void FidlDecoder::OnOutputPacket(fuchsia::mediacodec::CodecPacket packet,
                                 bool error_detected_before,
                                 bool error_detected_during) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  uint64_t buffer_lifetime_ordinal = packet.header.buffer_lifetime_ordinal;
  uint32_t packet_index = packet.header.packet_index;
  uint32_t buffer_index = packet.buffer_index;
  FXL_DCHECK(buffer_index != 0x80000000);

  // TODO(dustingreen): separate buffer_index from packet_index in FidlDecoder.
  // Until then, this will work for h264, but won't handle VP9 with its
  // show_existing_frame that can happen repeatedly for the same buffer.
  FXL_CHECK(packet_index == buffer_index);

  if (error_detected_before) {
    FXL_LOG(WARNING) << "OnOutputPacket: error_detected_before";
  }

  if (error_detected_during) {
    FXL_LOG(WARNING) << "OnOutputPacket: error_detected_during";
  }

  if (!output_buffers_.has_current_set()) {
    FXL_LOG(FATAL) << "OnOutputPacket event without prior OnOutputConfig event";
    // TODO(dalesat): Report error rather than crashing.
  }

  BufferSet& current_set = output_buffers_.current_set();

  if (packet.header.buffer_lifetime_ordinal != current_set.lifetime_ordinal()) {
    // Refers to an obsolete buffer. We've already assumed the outboard
    // decoder gave up this buffer, so there's no need to free it. Also, this
    // shouldn't happen, and there's no evidence that it does.
    FXL_LOG(FATAL) << "OnOutputPacket delivered tacket with obsolete "
                      "buffer_lifetime_ordinal.";
    return;
  }

  if (packet.stream_lifetime_ordinal != stream_lifetime_ordinal_) {
    // Refers to an obsolete stream. We'll just recycle the packet back to the
    // output decoder.
    outboard_decoder_->RecycleOutputPacket(std::move(packet.header));
    return;
  }

  FXL_DCHECK(buffer_lifetime_ordinal == current_set.lifetime_ordinal());
  fbl::RefPtr<PayloadBuffer> payload_buffer =
      current_set.TakeBufferFromDecoder(buffer_index);
  if (!payload_buffer) {
    FXL_LOG(FATAL) << "OnOutputPacket delivered packet using buffer that the "
                      "decoder didn't own.";
    return;
  }

  payload_buffer->AfterRecycling([this](PayloadBuffer* payload_buffer) {
    RecycleOutputPacket(payload_buffer);
  });

  // TODO(dalesat): Tolerate !has_timestamp_ish somehow.
  if (!packet.has_timestamp_ish) {
    FXL_LOG(ERROR) << "We demand has_timestamp_ish for now (TODO)";
    return;
  }

  next_pts_ = static_cast<int64_t>(packet.timestamp_ish);

  auto output_packet =
      Packet::Create(next_pts_, pts_rate_, true, false,
                     packet.valid_length_bytes, std::move(payload_buffer));

  if (revised_output_stream_type_) {
    output_packet->SetRevisedStreamType(std::move(revised_output_stream_type_));
  }

  stage()->PutOutputPacket(std::move(output_packet));
}

void FidlDecoder::OnOutputEndOfStream(uint64_t stream_lifetime_ordinal,
                                      bool error_detected_before) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (error_detected_before) {
    FXL_LOG(WARNING) << "OnOutputEndOfStream: error_detected_before";
  }

  stage()->PutOutputPacket(Packet::CreateEndOfStream(next_pts_, pts_rate_));
}

void FidlDecoder::OnFreeInputPacket(
    fuchsia::mediacodec::CodecPacketHeader packet_header) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  input_buffers_.ReleaseBufferForDecoder(packet_header.buffer_lifetime_ordinal,
                                         packet_header.packet_index);
}

void FidlDecoder::RecycleOutputPacket(PayloadBuffer* payload_buffer) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(payload_buffer);

  if (!output_buffers_.has_current_set() ||
      payload_buffer->buffer_config() !=
          output_buffers_.current_set().lifetime_ordinal()) {
    // This buffer is part of an obsolete set, so disregard it.
    return;
  }

  FXL_DCHECK(payload_buffer->buffer_config() ==
             output_buffers_.current_set().lifetime_ordinal());
  // Here we're creating a |PayloadBuffer| with a legit |RefPtr|, but the
  // |BufferSet| holds that |RefPtr| on the outboard decoder's behalf until
  // the decoder gives up ownership.
  output_buffers_.current_set().CreateBufferForDecoder(
      payload_buffer->id(), stage()->UseOutputVmos());

  outboard_decoder_->RecycleOutputPacket(fuchsia::mediacodec::CodecPacketHeader{
      .buffer_lifetime_ordinal = payload_buffer->buffer_config(),
      .packet_index = payload_buffer->id()});
}

void FidlDecoder::HandlePossibleOutputStreamTypeChange(
    const StreamType& old_type, const StreamType& new_type) {
  // TODO(dalesat): Actually compare the types.
  revised_output_stream_type_ = new_type.Clone();
}

}  // namespace media_player
