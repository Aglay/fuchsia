// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_PTS_MANAGER_H_
#define SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_PTS_MANAGER_H_

#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <cstdint>
#include <map>
#include <mutex>

class PtsManager {
 public:
  // 8 is the max number of frames in a VP9 superframe.  For H264, num_reorder_frames is max 16.  So
  // 32 should be enough for both VP9 and H264.
  static constexpr uint32_t kMaxEntriesDueToFrameReordering = 32;
  // Large enough to store an entry per every 4 bytes of the 4k h264 stream buffer.  This assumes
  // every frame is a 3 byte start code + 1 byte NALU header and that's all.  Real frames are
  // larger, so this will be enough entries for our current worst case.
  static constexpr uint32_t kMaxEntriesDueToH264SingleStreamBuffering = 4 * 1024 / 4;
  // This "extra" value should take care of any buffering in the video decoder itself, and any delay
  // outputting a decompressed frame after it has been removed from the stream buffer.
  static constexpr uint32_t kMaxEntriesDueToExtraDecoderDelay = 32;
  static constexpr uint32_t kMaxEntriesToKeep = kMaxEntriesDueToFrameReordering +
                                                kMaxEntriesDueToH264SingleStreamBuffering +
                                                kMaxEntriesDueToExtraDecoderDelay;
  class LookupResult {
   public:
    // Outside of PtsManager, can only be copied, not created from scratch and
    // not assigned.
    LookupResult(const LookupResult& from) = default;

    bool is_end_of_stream() const { return is_end_of_stream_; }
    bool has_pts() const { return has_pts_; }
    uint64_t pts() const { return pts_; }

   private:
    friend class PtsManager;
    LookupResult() = delete;

    LookupResult(bool is_end_of_stream, bool has_pts, uint64_t pts)
        : is_end_of_stream_(is_end_of_stream), has_pts_(has_pts), pts_(pts) {
      // PTS == 0 is valid, but if we don't have a PTS, the field must be set to
      // 0.  In other words, we still need the sparate has_pts_ to tell whether
      // we have a PTS when the pts field is 0 - this way all pts values are
      // usable.
      ZX_DEBUG_ASSERT(has_pts_ || !pts_);
      ZX_DEBUG_ASSERT(!(is_end_of_stream_ && has_pts_));
    }

    // If is_end_of_stream_, there is no PTS.  Instead, the stream is over.
    const bool is_end_of_stream_ = false;

    // If !has_pts_, the pts_ field is not meaningful (but is set to 0).
    const bool has_pts_ = false;

    // If has_pts(), the pts field is meaningful.
    //
    // When has_pts(), the PTS of the frame.
    // When !has_pts(), 0.
    const uint64_t pts_ = 0;
  };

  void SetLookupBitWidth(uint32_t lookup_bit_width);

  // Offset is the byte offset into the stream of the beginning of the frame.
  void InsertPts(uint64_t offset, bool has_pts, uint64_t pts);

  // |end_of_stream_offset| is the first byte offset which is not part of the
  // input stream data (stream offset of last input stream byte + 1).
  void SetEndOfStreamOffset(uint64_t end_of_stream_offset);

  // Offset must be within the frame that's being looked up.
  const LookupResult Lookup(uint64_t offset);

 private:
  // The last inserted offset is offset_to_result_.rbegin()->first, unless empty() in which case
  // logically 0.
  uint64_t GetLastInsertedOffset() __TA_REQUIRES(lock_);

  std::mutex lock_;
  __TA_GUARDED(lock_)
  uint32_t lookup_bit_width_ = 64;

  // TODO(dustingreen): Consider switching to a SortedCircularBuffer (to be implemented) of size
  // kMaxEntries instead, to avoid so many pointers and separate heap allocations.  Despite the
  // memory inefficiency vs. a circular buffer, this likely consumes ~128KiB, so switching isn't
  // urgent.
  __TA_GUARDED(lock_)
  std::map<uint64_t, LookupResult> offset_to_result_;
};

#endif  // SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_PTS_MANAGER_H_
