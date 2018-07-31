// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "codec_adapter_mpeg2.h"

#include <lib/fxl/logging.h>

CodecAdapterMpeg2::CodecAdapterMpeg2(std::mutex& lock,
                                     CodecAdapterEvents* codec_adapter_events,
                                     AmlogicVideo* video)
    : CodecAdapter(lock, codec_adapter_events), video_(video) {
  FXL_DCHECK(false) << "not yet implemented";
  (void)video_;
}

CodecAdapterMpeg2::~CodecAdapterMpeg2() {
  FXL_DCHECK(false) << "not yet implemented - power off probably can go here";
}

bool CodecAdapterMpeg2::IsCoreCodecRequiringOutputConfigForFormatDetection() {
  return false;
}

void CodecAdapterMpeg2::CoreCodecInit(
    const fuchsia::mediacodec::CodecFormatDetails&
        initial_input_format_details) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecStartStream(
    std::unique_lock<std::mutex>& lock) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecQueueInputFormatDetails(
    const fuchsia::mediacodec::CodecFormatDetails&
        per_stream_override_format_details) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecQueueInputPacket(const CodecPacket* packet) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecQueueInputEndOfStream() {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecStopStream(
    std::unique_lock<std::mutex>& lock) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecAddBuffer(CodecPort port,
                                           const CodecBuffer* buffer) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecConfigureBuffers(CodecPort port) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecRecycleOutputPacketLocked(
    CodecPacket* packet) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecEnsureBuffersNotConfiguredLocked(
    CodecPort port) {
  FXL_DCHECK(false) << "not yet implemented";
}

std::unique_ptr<const fuchsia::mediacodec::CodecOutputConfig>
CodecAdapterMpeg2::CoreCodecBuildNewOutputConfig(
    uint64_t stream_lifetime_ordinal,
    uint64_t new_output_buffer_constraints_version_ordinal,
    uint64_t new_output_format_details_version_ordinal,
    bool buffer_constraints_action_required) {
  FXL_DCHECK(false) << "not yet implemented";
  return std::make_unique<const fuchsia::mediacodec::CodecOutputConfig>();
}

void CodecAdapterMpeg2::CoreCodecMidStreamOutputBufferReConfigPrepare(
    std::unique_lock<std::mutex>& lock) {
  FXL_DCHECK(false) << "not yet implemented";
}

void CodecAdapterMpeg2::CoreCodecMidStreamOutputBufferReConfigFinish(
    std::unique_lock<std::mutex>& lock) {
  FXL_DCHECK(false) << "not yet implemented";
}
