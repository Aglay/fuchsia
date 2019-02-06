// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/overnet/protocol/ack_frame.h"
#include <assert.h>
#include "garnet/lib/overnet/protocol/varint.h"

namespace overnet {

AckFrame::Writer::Writer(const AckFrame* ack_frame)
    : ack_frame_(ack_frame),
      ack_to_seq_length_(varint::WireSizeFor(ack_frame_->ack_to_seq_)),
      delay_and_flags_length_(
          varint::WireSizeFor(ack_frame_->DelayAndFlags())) {
  wire_length_ = ack_to_seq_length_ + delay_and_flags_length_;
  nack_length_.reserve(ack_frame_->nack_seqs_.size());
  uint64_t base = ack_frame_->ack_to_seq_;
  for (auto n : ack_frame_->nack_seqs_) {
    auto enc = base - n;
    auto l = varint::WireSizeFor(enc);
    wire_length_ += l;
    nack_length_.push_back(l);
    base = n;
  }
  assert(ack_frame->WrittenLength() == wire_length_);
}

uint64_t AckFrame::WrittenLength() const {
  uint64_t wire_length =
      varint::WireSizeFor(ack_to_seq_) + varint::WireSizeFor(DelayAndFlags());
  uint64_t base = ack_to_seq_;
  for (auto n : nack_seqs_) {
    wire_length += varint::WireSizeFor(base - n);
    base = n;
  }
  return wire_length;
}

uint8_t* AckFrame::Writer::Write(uint8_t* out) const {
  uint8_t* p = out;
  p = varint::Write(ack_frame_->ack_to_seq_, ack_to_seq_length_, p);
  p = varint::Write(ack_frame_->DelayAndFlags(), delay_and_flags_length_, p);
  uint64_t base = ack_frame_->ack_to_seq_;
  for (size_t i = 0; i < nack_length_.size(); i++) {
    auto n = ack_frame_->nack_seqs_[i];
    auto enc = base - n;
    p = varint::Write(enc, nack_length_[i], p);
    base = n;
  }
  assert(p == out + wire_length_);
  return p;
}

uint64_t AckFrame::DelayAndFlags() const {
  uint64_t delay_part =
      (ack_delay_us_ >> 63) ? 0xffff'ffff'ffff'fffe : (ack_delay_us_ << 1);
  uint64_t partial_part = partial_ ? 1 : 0;
  return delay_part | partial_part;
}

StatusOr<AckFrame> AckFrame::Parse(Slice slice) {
  const uint8_t* bytes = slice.begin();
  const uint8_t* end = slice.end();
  uint64_t ack_to_seq;
  if (!varint::Read(&bytes, end, &ack_to_seq)) {
    return StatusOr<AckFrame>(StatusCode::INVALID_ARGUMENT,
                              "Failed to parse ack_to_seq from ack frame");
  }
  if (ack_to_seq == 0) {
    return StatusOr<AckFrame>(StatusCode::INVALID_ARGUMENT,
                              "Ack frame cannot ack_to_seq 0");
  }
  uint64_t delay_and_flags;
  if (!varint::Read(&bytes, end, &delay_and_flags)) {
    return StatusOr<AckFrame>(StatusCode::INVALID_ARGUMENT,
                              "Failed to parse delay_and_flags from ack frame");
  }
  bool is_partial = (delay_and_flags & 1) != 0;
  uint64_t ack_delay_us = delay_and_flags >> 1;
  AckFrame frame(ack_to_seq, ack_delay_us);
  frame.partial_ = is_partial;
  uint64_t base = ack_to_seq;
  while (bytes != end) {
    uint64_t offset;
    if (!varint::Read(&bytes, end, &offset)) {
      return StatusOr<AckFrame>(StatusCode::INVALID_ARGUMENT,
                                "Failed to read nack offset from ack frame");
    }
    if (offset >= base) {
      return StatusOr<AckFrame>(StatusCode::INVALID_ARGUMENT,
                                "Failed to read nack");
    }
    const uint64_t seq = base - offset;
    frame.AddNack(seq);
    base = seq;
  }
  return StatusOr<AckFrame>(std::move(frame));
}

std::ostream& operator<<(std::ostream& out, const AckFrame& ack_frame) {
  out << "ACK{to:" << ack_frame.ack_to_seq()
      << ", delay:" << ack_frame.ack_delay_us()
      << "us, partial:" << (ack_frame.partial() ? "yes" : "no") << ", nack=[";
  for (auto n : ack_frame.nack_seqs()) {
    out << n << ",";
  }
  return out << "]}";
}

}  // namespace overnet
