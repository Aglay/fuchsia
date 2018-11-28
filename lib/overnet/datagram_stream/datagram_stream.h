// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <queue>  // TODO(ctiller): switch to a short queue (inlined 1-2 elems, linked list)
#include "garnet/lib/overnet/datagram_stream/linearizer.h"
#include "garnet/lib/overnet/datagram_stream/receive_mode.h"
#include "garnet/lib/overnet/environment/timer.h"
#include "garnet/lib/overnet/environment/trace.h"
#include "garnet/lib/overnet/labels/reliability_and_ordering.h"
#include "garnet/lib/overnet/labels/seq_num.h"
#include "garnet/lib/overnet/packet_protocol/packet_protocol.h"
#include "garnet/lib/overnet/routing/router.h"
#include "garnet/lib/overnet/vocabulary/internal_list.h"
#include "garnet/lib/overnet/vocabulary/slice.h"

//#define OVERNET_TRACE_STATEREF_REFCOUNT

namespace overnet {

class MessageFragment {
 public:
  enum class Type : uint8_t { Chunk = 0, MessageCancel = 1, StreamEnd = 2 };

  MessageFragment(const MessageFragment&) = delete;
  MessageFragment& operator=(const MessageFragment&) = delete;

  MessageFragment(uint64_t message, Chunk chunk)
      : message_(message), type_(Type::Chunk) {
    assert(message > 0);
    new (&payload_.chunk) Chunk(std::move(chunk));
  }

  MessageFragment(MessageFragment&& other)
      : message_(other.message_), type_(other.type_) {
    switch (type_) {
      case Type::Chunk:
        new (&payload_.chunk) Chunk(std::move(other.payload_.chunk));
        break;
      case Type::MessageCancel:
      case Type::StreamEnd:
        new (&payload_.status) Status(std::move(other.payload_.status));
        break;
    }
  }

  MessageFragment& operator=(MessageFragment&& other) {
    if (type_ != other.type_) {
      this->~MessageFragment();
      new (this) MessageFragment(std::forward<MessageFragment>(other));
    } else {
      switch (type_) {
        case Type::Chunk:
          payload_.chunk = std::move(other.payload_.chunk);
          break;
        case Type::MessageCancel:
        case Type::StreamEnd:
          payload_.status = std::move(other.payload_.status);
          break;
      }
    }
    return *this;
  }

  ~MessageFragment() {
    switch (type_) {
      case Type::Chunk:
        payload_.chunk.~Chunk();
        break;
      case Type::MessageCancel:
      case Type::StreamEnd:
        payload_.status.~Status();
        break;
    }
  }

  static MessageFragment Abort(uint64_t message_id, Status status) {
    return MessageFragment(message_id, Type::MessageCancel, std::move(status));
  }

  static MessageFragment EndOfStream(uint64_t last_message_id, Status status) {
    return MessageFragment(last_message_id, Type::StreamEnd, std::move(status));
  }

  Slice Write(Border desired_border) const;
  static StatusOr<MessageFragment> Parse(Slice incoming);

  Type type() const { return type_; }

  uint64_t message() const { return message_; }

  const Chunk& chunk() const {
    assert(type_ == Type::Chunk);
    return payload_.chunk;
  }
  Chunk* mutable_chunk() {
    assert(type_ == Type::Chunk);
    return &payload_.chunk;
  }

  const Status& status() const {
    assert(type_ == Type::MessageCancel || type_ == Type::StreamEnd);
    return payload_.status;
  }

 private:
  static constexpr uint8_t kFlagEndOfMessage = 0x80;
  static constexpr uint8_t kFlagTypeMask = 0x0f;
  static constexpr uint8_t kReservedFlags =
      static_cast<uint8_t>(~(kFlagTypeMask | kFlagEndOfMessage));

  MessageFragment(uint64_t message, Type type, Status status)
      : message_(message), type_(type) {
    assert(type == Type::MessageCancel || type == Type::StreamEnd);
    assert(message != 0);
    new (&payload_.status) Status(std::move(status));
  }

  uint64_t message_;
  union Payload {
    Payload() {}
    ~Payload() {}

    Chunk chunk;
    Status status;
  };
  Type type_;
  Payload payload_;
};

class DatagramStream : private Router::StreamHandler,
                       private PacketProtocol::PacketSender {
  class IncomingMessage {
   public:
    inline static constexpr auto kModule =
        Module::DATAGRAM_STREAM_INCOMING_MESSAGE;

    IncomingMessage(DatagramStream* stream)
        : linearizer_(2 * stream->packet_protocol_.mss()) {}

    void Pull(StatusOrCallback<Optional<Slice>>&& done) {
      ScopedModule<IncomingMessage> in_im(this);
      linearizer_.Pull(std::forward<StatusOrCallback<Optional<Slice>>>(done));
    }
    void PullAll(StatusOrCallback<std::vector<Slice>>&& done) {
      ScopedModule<IncomingMessage> in_im(this);
      linearizer_.PullAll(
          std::forward<StatusOrCallback<std::vector<Slice>>>(done));
    }

    [[nodiscard]] bool Push(Chunk&& chunk) {
      ScopedModule<IncomingMessage> in_im(this);
      return linearizer_.Push(std::forward<Chunk>(chunk));
    }

    void Close(const Status& status) {
      ScopedModule<IncomingMessage> in_im(this);
      linearizer_.Close(status);
    }

    bool IsComplete() const { return linearizer_.IsComplete(); }

    InternalListNode<IncomingMessage> incoming_link;

   private:
    Linearizer linearizer_;
  };

  struct SendState {
    enum State : uint8_t {
      OPEN,
      CLOSED_OK,
      CLOSED_WITH_ERROR,
    };
    State state = State::OPEN;
    int refs = 0;
  };
  using SendStateMap = std::unordered_map<uint64_t, SendState>;
  using SendStateIt = SendStateMap::iterator;

  class StateRef {
   public:
    StateRef() = delete;
    StateRef(DatagramStream* stream, SendStateIt op)
        : stream_(stream), op_(op) {
      ScopedModule<DatagramStream> in_dgs(stream_);
#ifdef OVERNET_TRACE_STATEREF_REFCOUNT
      OVERNET_TRACE(DEBUG) << "StateRef:" << op_->first << " ADD "
                           << op_->second.refs << " -> "
                           << (op_->second.refs + 1);
#endif
      op->second.refs++;
    }
    StateRef(const StateRef& other) : stream_(other.stream_), op_(other.op_) {
      ScopedModule<DatagramStream> in_dgs(stream_);
#ifdef OVERNET_TRACE_STATEREF_REFCOUNT
      OVERNET_TRACE(DEBUG) << "StateRef:" << op_->first << " ADD "
                           << op_->second.refs << " -> "
                           << (op_->second.refs + 1);
#endif
      op_->second.refs++;
    }
    StateRef& operator=(StateRef other) {
      other.Swap(this);
      return *this;
    }
    void Swap(StateRef* other) {
      std::swap(op_, other->op_);
      std::swap(stream_, other->stream_);
    }

    ~StateRef() {
      ScopedModule<DatagramStream> in_dgs(stream_);
#ifdef OVERNET_TRACE_STATEREF_REFCOUNT
      OVERNET_TRACE(DEBUG) << "StateRef:" << op_->first << " DEL "
                           << op_->second.refs << " -> "
                           << (op_->second.refs - 1);
#endif
      if (0 == --op_->second.refs) {
        stream_->message_state_.erase(op_);
        stream_->MaybeFinishClosing();
      }
    }

    void SetClosed(const Status& status);

    DatagramStream* stream() const { return stream_; }
    SendState::State state() const { return op_->second.state; }
    void set_state(SendState::State state) { op_->second.state = state; }
    uint64_t message_id() const { return op_->first; }

   private:
    DatagramStream* stream_;
    SendStateIt op_;
  };

 public:
  inline static constexpr auto kModule = Module::DATAGRAM_STREAM;

  DatagramStream(Router* router, NodeId peer,
                 ReliabilityAndOrdering reliability_and_ordering,
                 StreamId stream_id);
  ~DatagramStream();

  DatagramStream(const DatagramStream&) = delete;
  DatagramStream& operator=(const DatagramStream&) = delete;
  DatagramStream(DatagramStream&&) = delete;
  DatagramStream& operator=(DatagramStream&&) = delete;

  virtual void Close(const Status& status, Callback<void> quiesced);
  void Close(Callback<void> quiesced) {
    Close(Status::Ok(), std::move(quiesced));
  }
  void RouterClose(Callback<void> quiesced) override final {
    Close(Status::Cancelled(), std::move(quiesced));
  }

  class SendOp final : private StateRef {
   public:
    inline static constexpr auto kModule = Module::DATAGRAM_STREAM_SEND_OP;

    SendOp(DatagramStream* stream, uint64_t payload_length);
    ~SendOp();

    void Push(Slice item, Callback<void> started);
    void Close(const Status& status);

   private:
    const uint64_t payload_length_;
    uint64_t push_offset_ = 0;
  };

  class ReceiveOp final {
    friend class DatagramStream;

   public:
    inline static constexpr auto kModule = Module::DATAGRAM_STREAM_RECV_OP;

    explicit ReceiveOp(DatagramStream* stream);
    ~ReceiveOp() {
      if (!closed_) {
        Close(incoming_message_ && incoming_message_->IsComplete()
                  ? Status::Ok()
                  : Status::Cancelled());
      }
      assert(closed_);
    }
    ReceiveOp(const ReceiveOp& rhs) = delete;
    ReceiveOp& operator=(const ReceiveOp& rhs) = delete;

    void Pull(StatusOrCallback<Optional<Slice>> ready);
    void PullAll(StatusOrCallback<std::vector<Slice>> ready);
    void Close(const Status& status);

   private:
    DatagramStream* const stream_;
    IncomingMessage* incoming_message_ = nullptr;
    bool closed_ = false;
    StatusOrCallback<Optional<Slice>> pending_pull_;
    StatusOrCallback<std::vector<Slice>> pending_pull_all_;
    InternalListNode<ReceiveOp> waiting_link_;
  };

  NodeId peer() const { return peer_; }

 protected:
  // Must be called by derived classes, after construction and before any other
  // methods.
  void Register();

 private:
  void HandleMessage(SeqNum seq, TimeStamp received, Slice data) override final;
  void SendPacket(SeqNum seq, LazySlice data,
                  Callback<void> done) override final;
  void SendCloseAndFlushQuiesced(int retry_number);
  void FinishClosing();
  void MaybeFinishClosing();

  void MaybeContinueReceive();

  void SendChunk(StateRef state, Chunk chunk, Callback<void> started);
  void SendNextChunk();
  void SendError(StateRef state, const Status& status);
  void CompleteReliable(const Status& status, StateRef state, Chunk chunk);
  void CompleteUnreliable(const Status& status, StateRef state);
  void CancelReceives();
  std::string PendingSendString();

  Timer* const timer_;
  Router* const router_;
  const NodeId peer_;
  const StreamId stream_id_;
  const ReliabilityAndOrdering reliability_and_ordering_;
  uint64_t next_message_id_ = 1;
  uint64_t largest_incoming_message_id_seen_ = 0;
  receive_mode::ParameterizedReceiveMode receive_mode_;
  PacketProtocol packet_protocol_;
  enum class CloseState : uint8_t {
    OPEN,
    LOCAL_CLOSE_REQUESTED_OK,
    LOCAL_CLOSE_REQUESTED_WITH_ERROR,
    REMOTE_CLOSED,
    DRAINING_LOCAL_CLOSED_OK,
    CLOSING_PROTOCOL,
    CLOSED,
  };
  friend inline std::ostream& operator<<(std::ostream& out, CloseState state) {
    switch (state) {
      case CloseState::OPEN:
        return out << "OPEN";
      case CloseState::LOCAL_CLOSE_REQUESTED_OK:
        return out << "LOCAL_CLOSE_REQUESTED_OK";
      case CloseState::LOCAL_CLOSE_REQUESTED_WITH_ERROR:
        return out << "LOCAL_CLOSE_REQUESTED_WITH_ERROR";
      case CloseState::REMOTE_CLOSED:
        return out << "REMOTE_CLOSED";
      case CloseState::DRAINING_LOCAL_CLOSED_OK:
        return out << "DRAINING_LOCAL_CLOSED_OK";
      case CloseState::CLOSING_PROTOCOL:
        return out << "CLOSING_PROTOCOL";
      case CloseState::CLOSED:
        return out << "CLOSED";
    }
    return out << "UNKNOWN";
  }
  bool IsClosedForSending() {
    switch (close_state_) {
      case CloseState::LOCAL_CLOSE_REQUESTED_WITH_ERROR:
      case CloseState::REMOTE_CLOSED:
      case CloseState::CLOSED:
      case CloseState::CLOSING_PROTOCOL:
        return true;
      case CloseState::DRAINING_LOCAL_CLOSED_OK:
      case CloseState::LOCAL_CLOSE_REQUESTED_OK:
      case CloseState::OPEN:
        return false;
    }
  }
  CloseState close_state_ = CloseState::OPEN;
  Optional<Status> local_close_status_;
  struct RequestedClose {
    uint64_t last_message_id;
    Status status;
    bool operator==(const RequestedClose& other) const {
      return last_message_id == other.last_message_id &&
             status.code() == other.status.code();
    }
    bool operator!=(const RequestedClose& other) const {
      return !operator==(other);
    }
  };
  Optional<RequestedClose> requested_close_;

  struct ChunkAndState {
    Chunk chunk;
    StateRef state;
  };

  struct PendingSend {
    ChunkAndState what;
    Callback<void> started;
  };
  std::vector<PendingSend> pending_send_;
  bool sending_ = false;

  SendStateMap message_state_;

  // TODO(ctiller): a custom allocator here would be worthwhile, especially one
  // that could remove allocations for the common case of few entries.
  std::unordered_map<uint64_t, IncomingMessage> messages_;
  InternalList<IncomingMessage, &IncomingMessage::incoming_link>
      unclaimed_messages_;
  InternalList<ReceiveOp, &ReceiveOp::waiting_link_> unclaimed_receives_;

  std::vector<Callback<void>> on_quiesced_;
};

}  // namespace overnet
