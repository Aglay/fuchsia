// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_TEST_VMO_BACKED_BUFFER_H_
#define SRC_MEDIA_AUDIO_LIB_TEST_VMO_BACKED_BUFFER_H_

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/vmo.h>

#include <memory>

#include "src/media/audio/lib/format/format.h"
#include "src/media/audio/lib/test/audio_buffer.h"

namespace media::audio::test {

class VmoBackedBuffer {
 public:
  VmoBackedBuffer(const Format& format, size_t frame_count)
      : format_(format), frame_count_(frame_count) {}

  // Allocate an appropriately-sized VMO. The memory is initialized to all zeros.
  zx::vmo CreateAndMapVmo(bool writable_on_transfer) {
    FX_CHECK(BufferStart() == nullptr);

    auto rights = ZX_RIGHT_READ | ZX_RIGHT_MAP | ZX_RIGHT_TRANSFER;
    if (writable_on_transfer) {
      rights |= ZX_RIGHT_WRITE;
    }
    zx::vmo vmo;
    zx_status_t status = vmo_mapper_.CreateAndMap(SizeBytes(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                                  nullptr, &vmo, rights);
    FX_CHECK(status == ZX_OK) << "VmoMapper:::CreateAndMap failed: " << status;
    Clear();
    return vmo;
  }

  // Map a pre-allocated VMO into this buffer. The memory is initialized to all zeros.
  void MapVmo(const zx::vmo& vmo) {
    FX_CHECK(BufferStart() == nullptr);

    uint64_t vmo_size;
    zx_status_t status = vmo.get_size(&vmo_size);
    ASSERT_EQ(status, ZX_OK) << "VMO get_size failed: " << status;
    ASSERT_GE(vmo_size, SizeBytes())
        << "Buffer size " << SizeBytes() << " is greater than VMO size " << vmo_size;

    zx_vm_option_t flags = ZX_VM_PERM_READ | ZX_VM_PERM_WRITE;
    status = vmo_mapper_.Map(vmo, 0u, SizeBytes(), flags);
    ASSERT_EQ(status, ZX_OK) << "VmoMapper::Map failed: " << status;
    Clear();
  };

  // Reports whether the buffer has been allocated.
  bool IsValid() const { return BufferStart() != nullptr; }

  // Size of this payload buffer.
  size_t SizeBytes() const { return format_.bytes_per_frame() * frame_count_; }

  // Take a snapshot of the buffer.
  template <fuchsia::media::AudioSampleFormat SampleFormat>
  AudioBuffer<SampleFormat> Snapshot() {
    AudioBuffer<SampleFormat> out(format_, frame_count_);
    memmove(&out.samples[0], BufferStart(), SizeBytes());
    return out;
  }

  // Append a slice to the buffer, advancing the current seek position.
  template <fuchsia::media::AudioSampleFormat SampleFormat>
  void Append(AudioBufferSlice<SampleFormat> slice) {
    WriteAt(append_offset_frames_, slice);
    append_offset_frames_ += slice.NumFrames();
  }

  // Reset the buffer to all zeros and seek to the start of the buffer.
  void Clear() {
    memset(BufferStart(), 0, SizeBytes());
    append_offset_frames_ = 0;
  }

  // Write a slice to the given absolute offset.
  template <fuchsia::media::AudioSampleFormat SampleFormat>
  void WriteAt(size_t pos_in_frames, AudioBufferSlice<SampleFormat> slice) {
    FX_CHECK(pos_in_frames + slice.NumFrames() <= frame_count_);

    auto dst = BufferStart() + pos_in_frames * format_.bytes_per_frame();
    auto src = &slice.buf->samples[slice.SampleIndex(0, 0)];
    memmove(dst, src, slice.NumBytes());
  }

  // Set every sample to the given value.
  template <fuchsia::media::AudioSampleFormat SampleFormat>
  void Memset(typename SampleFormatTraits<SampleFormat>::SampleT value) {
    for (size_t k = 0; k < frame_count_ * format_.channels(); k++) {
      auto dst = reinterpret_cast<typename SampleFormatTraits<SampleFormat>::SampleT*>(
          BufferStart() + k * format_.bytes_per_sample());
      *dst = value;
    }
  }

 private:
  uint8_t* BufferStart() const { return reinterpret_cast<uint8_t*>(vmo_mapper_.start()); }

  const Format format_;
  const size_t frame_count_;

  fzl::VmoMapper vmo_mapper_;
  size_t append_offset_frames_ = 0;
};

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_LIB_TEST_VMO_BACKED_BUFFER_H_
