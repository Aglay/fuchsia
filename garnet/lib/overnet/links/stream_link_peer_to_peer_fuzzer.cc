// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/overnet/streamlinkfuzzer/cpp/fidl.h>
#include <queue>
#include "garnet/lib/overnet/links/stream_link.h"
#include "garnet/lib/overnet/protocol/fidl.h"
#include "garnet/lib/overnet/routing/router.h"
#include "garnet/lib/overnet/testing/test_timer.h"
#include "garnet/lib/overnet/testing/trace_cout.h"

using namespace overnet;

namespace {

class FuzzedStreamLink final : public StreamLink {
 public:
  FuzzedStreamLink(Router* router, NodeId peer)
      : StreamLink(router, peer, 64, 1), timer_(router->timer()) {}

  bool is_busy() const { return !done_.empty(); }

  void Emit(Slice slice, Callback<Status> done) override {
    if (!done_.empty()) {
      abort();
    }
    pending_.Append(slice);
    done_ = std::move(done);
  }

  void Done() {
    if (!done_.empty()) {
      done_(Status::Ok());
    }
  }

  void Flush(uint64_t bytes) {
    auto pending_length = pending_.length();
    if (bytes == 0) {
    } else if (bytes >= pending_length) {
      partner_->Process(timer_->Now(), std::move(pending_));
    } else {
      partner_->Process(timer_->Now(), pending_.TakeUntilOffset(bytes));
    }
  }

  void set_partner(FuzzedStreamLink* partner) { partner_ = partner; }

 private:
  Timer* const timer_;
  Slice pending_;
  Callback<Status> done_;
  FuzzedStreamLink* partner_ = nullptr;
};

class FuzzedHandler final : public Router::StreamHandler {
 public:
  ~FuzzedHandler() {
    if (!expectations_.empty()) {
      abort();
    }
  }

  void Expect(Slice slice) { expectations_.emplace(std::move(slice)); }

  void RouterClose(Callback<void> quiesced) override {
    if (!expectations_.empty()) {
      abort();
    }
  }
  void HandleMessage(SeqNum seq, TimeStamp received, Slice data) override {
    if (data != expectations_.front()) {
      abort();
    }
    expectations_.pop();
  }

 private:
  std::queue<Slice> expectations_;
};

class StreamLinkFuzzer {
 public:
  StreamLinkFuzzer(bool log_stuff)
      : logging_(log_stuff ? new Logging(&timer_) : nullptr) {
    auto link = MakeLink<FuzzedStreamLink>(&router_1_, NodeId(2));
    link_12_ = link.get();
    router_1_.RegisterLink(std::move(link));

    link = MakeLink<FuzzedStreamLink>(&router_2_, NodeId(1));
    link_21_ = link.get();
    router_2_.RegisterLink(std::move(link));

    link_12_->set_partner(link_21_);
    link_21_->set_partner(link_12_);

    router_2_.RegisterStream(NodeId(1), StreamId(1), &handler_1_).MustSucceed();
    router_1_.RegisterStream(NodeId(2), StreamId(1), &handler_2_).MustSucceed();
  }

  ~StreamLinkFuzzer() {
    link_12_->Flush(std::numeric_limits<size_t>::max());
    link_21_->Flush(std::numeric_limits<size_t>::max());
    link_12_->Done();
    link_21_->Done();

    router_2_.UnregisterStream(NodeId(1), StreamId(1), &handler_1_)
        .MustSucceed();
    router_1_.UnregisterStream(NodeId(2), StreamId(1), &handler_2_)
        .MustSucceed();

    int waiting = 2;
    router_1_.Close([&] { waiting--; });
    router_2_.Close([&] { waiting--; });
    while (waiting) {
      timer_.StepUntilNextEvent();
    }
  }

  void Run(fuchsia::overnet::streamlinkfuzzer::PeerToPeerPlan plan) {
    using namespace fuchsia::overnet::streamlinkfuzzer;
    for (const auto& action : plan.actions) {
      if (!valid_node(action.node)) {
        continue;
      }
      switch (action.type.Which()) {
        case PeerToPeerActionType::Tag::Empty:
          break;
        case PeerToPeerActionType::Tag::kSendPacket: {
          auto* lnk = link(action.node);
          auto packet = Slice::FromContainer(action.type.send_packet());
          if (!lnk->is_busy()) {
            handler(action.node)->Expect(packet);
          }
          auto cur_seq = seq_;
          lnk->Forward(Message::SimpleForwarder(
              std::move(RoutableMessage(src(action.node))
                            .AddDestination(dst(action.node), StreamId(1),
                                            SeqNum(seq_++, cur_seq))),
              std::move(packet), timer_.Now()));
        } break;
        case PeerToPeerActionType::Tag::kSentPacket:
          link(action.node)->Done();
          break;
        case PeerToPeerActionType::Tag::kAllowBytes:
          link(action.node)->Flush(action.type.allow_bytes());
          break;
      }
      timer_.Step(1);
    }
  }

 private:
  bool valid_node(fuchsia::overnet::streamlinkfuzzer::NodeId id) {
    switch (id) {
      case fuchsia::overnet::streamlinkfuzzer::NodeId::A:
      case fuchsia::overnet::streamlinkfuzzer::NodeId::B:
        return true;
      default:
        return false;
    }
  }
  FuzzedStreamLink* link(fuchsia::overnet::streamlinkfuzzer::NodeId id) {
    switch (id) {
      case fuchsia::overnet::streamlinkfuzzer::NodeId::A:
        return link_12_;
      case fuchsia::overnet::streamlinkfuzzer::NodeId::B:
        return link_21_;
    }
  }
  FuzzedHandler* handler(fuchsia::overnet::streamlinkfuzzer::NodeId id) {
    switch (id) {
      case fuchsia::overnet::streamlinkfuzzer::NodeId::A:
        return &handler_1_;
      case fuchsia::overnet::streamlinkfuzzer::NodeId::B:
        return &handler_2_;
    }
  }
  Router* router(fuchsia::overnet::streamlinkfuzzer::NodeId id) {
    switch (id) {
      case fuchsia::overnet::streamlinkfuzzer::NodeId::A:
        return &router_1_;
      case fuchsia::overnet::streamlinkfuzzer::NodeId::B:
        return &router_2_;
    }
  }
  NodeId src(fuchsia::overnet::streamlinkfuzzer::NodeId id) {
    switch (id) {
      case fuchsia::overnet::streamlinkfuzzer::NodeId::A:
        return NodeId(1);
      case fuchsia::overnet::streamlinkfuzzer::NodeId::B:
        return NodeId(2);
    }
  }
  NodeId dst(fuchsia::overnet::streamlinkfuzzer::NodeId id) {
    switch (id) {
      case fuchsia::overnet::streamlinkfuzzer::NodeId::A:
        return NodeId(2);
      case fuchsia::overnet::streamlinkfuzzer::NodeId::B:
        return NodeId(1);
    }
  }

  TestTimer timer_;
  struct Logging {
    Logging(Timer* timer) : tracer(timer) {}
    TraceCout tracer;
    ScopedRenderer set_tracer{&tracer};
  };
  std::unique_ptr<Logging> logging_;
  Router router_1_{&timer_, NodeId(1), false};
  Router router_2_{&timer_, NodeId(2), false};
  FuzzedStreamLink* link_12_;
  FuzzedStreamLink* link_21_;
  uint64_t seq_ = 1;
  FuzzedHandler handler_1_;
  FuzzedHandler handler_2_;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (auto buffer = Decode<fuchsia::overnet::streamlinkfuzzer::PeerToPeerPlan>(
          Slice::FromCopiedBuffer(data, size));
      buffer.is_ok()) {
    StreamLinkFuzzer(false).Run(std::move(*buffer));
  }
  return 0;
}
