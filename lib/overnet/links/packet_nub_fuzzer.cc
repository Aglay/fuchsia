// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/overnet/links/packet_nub_fuzzer.h"

namespace overnet {

PacketNubFuzzer::PacketNubFuzzer(bool logging)
    : logging_(logging ? new Logging(&timer_) : nullptr) {}

void PacketNubFuzzer::Process(uint64_t src, Slice slice) {
  nub_.Process(timer_.Now(), src, std::move(slice));
}

bool PacketNubFuzzer::StepTime(uint64_t microseconds) {
  timer_.Step(microseconds);
  return timer_.Now().after_epoch() != TimeDelta::PositiveInf();
}

void PacketNubFuzzer::Budget::AddBudget(uint64_t address, uint64_t bytes) {
  budget_[address].push(bytes);
}

void PacketNubFuzzer::Budget::ConsumeBudget(uint64_t address, uint64_t bytes) {
  auto& queue = budget_[address];
  assert(!queue.empty());
  // The bytes allocated in the budget are for the next packet sent: if those
  // bytes are not used, they do not stack.
  assert(bytes <= queue.front());
  queue.pop();
}

PacketNubFuzzer::Nub::Nub(Timer* timer)
    : BaseNub(timer, NodeId(1)), router_(timer, NodeId(1), false) {}

void PacketNubFuzzer::Nub::Process(TimeStamp received, uint64_t src,
                                   Slice slice) {
  if (!HasConnectionTo(src)) {
    budget_.AddBudget(src, slice.length());
  }
  BaseNub::Process(received, src, std::move(slice));
}

void PacketNubFuzzer::Nub::SendTo(uint64_t dest, Slice slice) {
  if (!HasConnectionTo(dest)) {
    budget_.ConsumeBudget(dest, slice.length());
  }
}

Router* PacketNubFuzzer::Nub::GetRouter() { return &router_; }

void PacketNubFuzzer::Nub::Publish(LinkPtr<> link) {
  auto node = link->GetLinkMetrics().label()->to;
  assert(NodeId(node) != NodeId(1));
  router_.RegisterLink(std::move(link));
}

PacketNubFuzzer::Logging::Logging(Timer* timer) : tracer(timer) {}

}  // namespace overnet
