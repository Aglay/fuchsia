// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/examples/media/wav_record/wav_recorder.h"

#include <fbl/auto_call.h>
#include <fcntl.h>
#include <lib/async-loop/loop.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lib/fxl/logging.h"
#include "lib/media/audio/types.h"

#include "garnet/lib/media/wav_writer/wav_writer.h"

namespace examples {

constexpr zx_duration_t kCaptureChunkDuration = ZX_MSEC(100);
constexpr size_t kCaptureChunkCount = 10;
constexpr uint32_t kMinChannels = 1;
constexpr uint32_t kMaxChannels = 8;

static const std::string kShowUsageOption1 = "?";
static const std::string kShowUsageOption2 = "help";
static const std::string kVerboseOption = "v";
static const std::string kLoopbackOption = "loopback";
static const std::string kAsyncModeOption = "async-mode";
static const std::string kFloatFormatOption = "float";
static const std::string k24In32FormatOption = "int24";
static const std::string kPacked24FormatOption = "packed24";
static const std::string kFrameRateOption = "frame-rate";
static const std::string kChannelsOption = "channels";

WavRecorder::~WavRecorder() {
  if (payload_buf_virt_ != nullptr) {
    FXL_DCHECK(payload_buf_size_ != 0);
    FXL_DCHECK(bytes_per_frame_ != 0);
    zx::vmar::root_self()->unmap(reinterpret_cast<uintptr_t>(payload_buf_virt_),
                                 payload_buf_size_);
  }
}

void WavRecorder::Run(component::StartupContext* app_context) {
  auto cleanup = fbl::MakeAutoCall([this]() { Shutdown(); });
  const auto& pos_args = cmd_line_.positional_args();

  // Parse our args.
  if (cmd_line_.HasOption(kShowUsageOption1) ||
      cmd_line_.HasOption(kShowUsageOption2)) {
    Usage();
    return;
  }

  verbose_ = cmd_line_.HasOption(kVerboseOption);
  loopback_ = cmd_line_.HasOption(kLoopbackOption);

  if (pos_args.size() < 1) {
    Usage();
    return;
  }

  filename_ = pos_args[0].c_str();

  // Connect to the audio service and obtain a capturer
  fuchsia::media::AudioPtr audio =
      app_context->ConnectToEnvironmentService<fuchsia::media::Audio>();

  audio->CreateCapturer(capturer_.NewRequest(), loopback_);
  capturer_.set_error_handler([this]() {
    FXL_LOG(ERROR) << "Connection lost unexpectedly, shutting down.";
    Shutdown();
  });

  // Fetch the initial media type and figure out what we need to do from there.
  capturer_->GetStreamType([this](fuchsia::media::StreamType type) {
    OnDefaultFormatFetched(std::move(type));
  });

  // Quit if someone hits a key.
  keystroke_waiter_.Wait([this](zx_status_t, uint32_t) { OnQuit(); },
                         STDIN_FILENO, POLLIN);

  cleanup.cancel();
}

void WavRecorder::Usage() {
  printf("Usage: %s [options] <filename>\n", cmd_line_.argv0().c_str());
  printf("  --%s : be verbose\n", kVerboseOption.c_str());
  printf("  --%s : record from loopback\n", kLoopbackOption.c_str());
  printf("  --%s : capture using 'async-mode'\n", kAsyncModeOption.c_str());
  printf("\n    Default is to record and save as 16-bit integer\n");
  printf("  --%s : record and save as 32-bit float\n",
         kFloatFormatOption.c_str());
  printf("  --%s : record and save as 24-in-32 int (left-justify)\n",
         k24In32FormatOption.c_str());
  printf("  --%s : record as 24-in-32 int (left-justify), save as packed-24\n",
         kPacked24FormatOption.c_str());
  printf("\n  --%s=<rate> : frame rate at which to capture (range [%u, %u])\n",
         kFrameRateOption.c_str(), fuchsia::media::MIN_PCM_FRAMES_PER_SECOND,
         fuchsia::media::MAX_PCM_FRAMES_PER_SECOND);
  printf("  --%s=<count> : number of channels to capture (range [%u, %u])\n",
         kChannelsOption.c_str(), kMinChannels, kMaxChannels);
}

void WavRecorder::Shutdown() {
  if (capturer_.is_bound()) {
    capturer_.set_error_handler(nullptr);
    capturer_.Unbind();
  }

  if (clean_shutdown_) {
    if (wav_writer_.Close()) {
      printf("done.\n");
    } else {
      printf("file close failed.\n");
    }
  } else {
    if (!wav_writer_.Delete()) {
      printf("Could not delete WAV file.\n");
    }
  }

  quit_callback_();
}

bool WavRecorder::SetupPayloadBuffer() {
  capture_frames_per_chunk_ =
      (kCaptureChunkDuration * frames_per_second_) / ZX_SEC(1);
  payload_buf_frames_ = capture_frames_per_chunk_ * kCaptureChunkCount;
  payload_buf_size_ = payload_buf_frames_ * bytes_per_frame_;

  zx_status_t res;
  res = zx::vmo::create(payload_buf_size_, 0, &payload_buf_vmo_);
  if (res != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to create " << payload_buf_size_
                   << " byte payload buffer (res " << res << ")";
    return false;
  }

  uintptr_t tmp;
  res = zx::vmar::root_self()->map(0, payload_buf_vmo_, 0, payload_buf_size_,
                                   ZX_VM_FLAG_PERM_READ, &tmp);
  if (res != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to map " << payload_buf_size_
                   << " byte payload buffer (res " << res << ")";
    return false;
  }
  payload_buf_virt_ = reinterpret_cast<void*>(tmp);

  return true;
}

void WavRecorder::SendCaptureJob() {
  FXL_DCHECK(capture_frame_offset_ < payload_buf_frames_);
  FXL_DCHECK((capture_frame_offset_ + capture_frames_per_chunk_) <=
             payload_buf_frames_);

  // clang-format off
  capturer_->CaptureAt(
      capture_frame_offset_,
      capture_frames_per_chunk_,
      [this](fuchsia::media::MediaPacket packet) {
        OnPacketCaptured(std::move(packet));
      });
  // clang-format on

  capture_frame_offset_ += capture_frames_per_chunk_;
  if (capture_frame_offset_ >= payload_buf_frames_) {
    capture_frame_offset_ = 0u;
  }
}

void WavRecorder::OnDefaultFormatFetched(fuchsia::media::StreamType type) {
  auto cleanup = fbl::MakeAutoCall([this]() { Shutdown(); });
  zx_status_t res;

  if (!type.medium_specific.is_audio()) {
    FXL_LOG(ERROR) << "default format is not audio!";
    return;
  }

  const auto& fmt = type.medium_specific.audio();

  // If user erroneously specifies float AND 24-in-32, prefer float.
  if (cmd_line_.HasOption(kFloatFormatOption)) {
    sample_format_ = fuchsia::media::AudioSampleFormat::FLOAT;
  } else if (cmd_line_.HasOption(kPacked24FormatOption)) {
    pack_24bit_samples_ = true;
    sample_format_ = fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32;
  } else if (cmd_line_.HasOption(k24In32FormatOption)) {
    sample_format_ = fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32;
  } else {
    sample_format_ = fuchsia::media::AudioSampleFormat::SIGNED_16;
  }

  channel_count_ = fmt.channels;
  frames_per_second_ = fmt.frames_per_second;

  bool change_format = false;

  if (fmt.sample_format != sample_format_) {
    change_format = true;
  }

  std::string opt;
  if (cmd_line_.GetOptionValue(kFrameRateOption, &opt)) {
    uint32_t rate;
    if (::sscanf(opt.c_str(), "%u", &rate) != 1) {
      Usage();
      return;
    }

    if ((rate < fuchsia::media::MIN_PCM_FRAMES_PER_SECOND) ||
        (rate > fuchsia::media::MAX_PCM_FRAMES_PER_SECOND)) {
      printf("Frame rate (%u) must be within range [%u, %u]\n", rate,
             fuchsia::media::MIN_PCM_FRAMES_PER_SECOND,
             fuchsia::media::MAX_PCM_FRAMES_PER_SECOND);
      return;
    }

    if (frames_per_second_ != rate) {
      frames_per_second_ = rate;
      change_format = true;
    }
  }

  if (cmd_line_.GetOptionValue(kChannelsOption, &opt)) {
    uint32_t count;
    if (::sscanf(opt.c_str(), "%u", &count) != 1) {
      Usage();
      return;
    }

    if ((count < kMinChannels) || (count > kMaxChannels)) {
      printf("Channel count (%u) must be within range [%u, %u]\n", count,
             kMinChannels, kMaxChannels);
      return;
    }

    if (channel_count_ != count) {
      channel_count_ = count;
      change_format = true;
    }
  }

  uint32_t bytes_per_sample =
      (sample_format_ == fuchsia::media::AudioSampleFormat::FLOAT)
          ? sizeof(float)
          : (sample_format_ ==
             fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32)
                ? sizeof(int32_t)
                : sizeof(int16_t);
  bytes_per_frame_ = channel_count_ * bytes_per_sample;
  uint32_t bits_per_sample = bytes_per_sample * 8;
  if (sample_format_ == fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32 &&
      pack_24bit_samples_ == true) {
    bits_per_sample = 24;
  }

  // Write the inital WAV header
  if (!wav_writer_.Initialize(filename_, sample_format_, channel_count_,
                              frames_per_second_, bits_per_sample)) {
    return;
  }

  // If our desired format is different from the default capturer format, change
  // formats now.
  if (change_format) {
    fuchsia::media::MediumSpecificStreamType medium_specific_stream_type;
    medium_specific_stream_type.set_audio(media::CreateAudioStreamType(
        sample_format_, channel_count_, frames_per_second_));

    fuchsia::media::StreamType stream_type;
    stream_type.medium_specific = std::move(medium_specific_stream_type);
    stream_type.encoding = fuchsia::media::AUDIO_ENCODING_LPCM;
    capturer_->SetStreamType(std::move(stream_type));
  }

  // Record at unity gain.
  if (change_format) {
    capturer_->SetGain(0.0f);
  }

  // Create our shared payload buffer, map it into place, then dup the handle
  // and pass it on to the capturer to fill.
  if (!SetupPayloadBuffer()) {
    return;
  }

  zx::vmo capturer_vmo;
  res = payload_buf_vmo_.duplicate(
      ZX_RIGHT_TRANSFER | ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_MAP,
      &capturer_vmo);
  if (res != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to duplicate VMO handle (res " << res << ")";
    return;
  }
  capturer_->SetPayloadBuffer(std::move(capturer_vmo));

  // Are we operating in synchronous or asynchronous mode?  If synchronous,
  // Queue up all of our capture buffers using to get the ball rolling.
  // Otherwise, send a handle to our AudioCapturerClient interface and start to
  // operate in async mode.
  if (!cmd_line_.HasOption(kAsyncModeOption)) {
    for (size_t i = 0; i < kCaptureChunkCount; ++i) {
      SendCaptureJob();
    }
  } else {
    FXL_DCHECK(payload_buf_frames_);
    FXL_DCHECK(capture_frames_per_chunk_);
    FXL_DCHECK((payload_buf_frames_ % capture_frames_per_chunk_) == 0);
    capturer_.events().OnPacketCaptured =
        [this](fuchsia::media::MediaPacket pkt) {
          OnPacketCaptured(std::move(pkt));
        };
    capturer_->StartAsyncCapture(capture_frames_per_chunk_);
  }

  if (sample_format_ == fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32) {
    FXL_DCHECK(bits_per_sample == (pack_24bit_samples_ ? 24 : 32));
    if (pack_24bit_samples_ == true) {
      compress_32_24_buff_ =
          std::make_unique<uint8_t[]>(payload_buf_size_ * 3 / 4);
    }
  }

  printf(
      "Recording %s, %u Hz, %u channel linear PCM from %s into '%s'\n",
      sample_format_ == fuchsia::media::AudioSampleFormat::FLOAT
          ? "32-bit float"
          : sample_format_ == fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32
                ? (pack_24bit_samples_ ? "packed 24-bit signed int"
                                       : "24-bit-in-32-bit signed int")
                : "16-bit signed int",
      frames_per_second_, channel_count_,
      loopback_ ? "loopback" : "default input", filename_);

  cleanup.cancel();
}

void WavRecorder::OnPacketCaptured(fuchsia::media::MediaPacket pkt) {
  if (verbose_) {
    printf("PACKET [%6lu, %6lu] flags 0x%02x : ts %ld\n", pkt.payload_offset,
           pkt.payload_size, pkt.flags, pkt.pts);
  }

  FXL_DCHECK((pkt.payload_offset + pkt.payload_size) <=
             (payload_buf_frames_ * bytes_per_frame_));

  if (pkt.payload_size) {
    FXL_DCHECK(payload_buf_virt_);

    auto tgt =
        reinterpret_cast<uint8_t*>(payload_buf_virt_) + pkt.payload_offset;

    uint32_t write_size = pkt.payload_size;
    // If 24_in_32, write as packed-24, skipping the first, least-significant of
    // each four bytes). Assuming Write does not buffer, compress locally and
    // call Write just once, to avoid potential performance problems.
    if (sample_format_ == fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32 &&
        pack_24bit_samples_) {
      uint32_t write_idx = 0;
      uint32_t read_idx = 0;
      while (read_idx < pkt.payload_size) {
        ++read_idx;
        compress_32_24_buff_[write_idx++] = tgt[read_idx++];
        compress_32_24_buff_[write_idx++] = tgt[read_idx++];
        compress_32_24_buff_[write_idx++] = tgt[read_idx++];
      }
      write_size = write_idx;
      tgt = compress_32_24_buff_.get();
    }

    if (!wav_writer_.Write(reinterpret_cast<void* const>(tgt), write_size)) {
      printf("File write failed. Trying to save any already-written data.\n");
      if (!wav_writer_.Close()) {
        printf("File close failed as well.\n");
      }
      Shutdown();
    }
  }

  if (!clean_shutdown_ && (capturer_.events().OnPacketCaptured == nullptr)) {
    SendCaptureJob();
  } else if (pkt.flags & fuchsia::media::kFlagEos) {
    Shutdown();
  }
}

void WavRecorder::OnQuit() {
  printf("Shutting down...\n");
  clean_shutdown_ = true;

  if (capturer_.events().OnPacketCaptured != nullptr) {
    capturer_->StopAsyncCapture();
  } else {
    capturer_->Flush();
  }
}

}  // namespace examples
