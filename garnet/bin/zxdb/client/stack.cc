// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/stack.h"

#include <map>

#include "garnet/bin/zxdb/client/frame.h"
#include "garnet/bin/zxdb/client/frame_fingerprint.h"
#include "garnet/bin/zxdb/common/err.h"
#include "garnet/bin/zxdb/expr/expr_eval_context.h"
#include "garnet/bin/zxdb/symbols/function.h"
#include "garnet/lib/debug_ipc/helper/message_loop.h"
#include "garnet/lib/debug_ipc/records.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"

namespace zxdb {

namespace {

// Implementation of Frame for inlined frames. Inlined frames have a different
// location in the source code, but refer to the underlying physical frame for
// most data.
class InlineFrame final : public Frame {
 public:
  // The physical_frame must outlive this class. Normally both are owned by the
  // Stack and have the same lifetime.
  InlineFrame(Frame* physical_frame, Location loc)
      : Frame(physical_frame->session()),
        physical_frame_(physical_frame),
        location_(loc) {}
  ~InlineFrame() override = default;

  // Frame implementation.
  Thread* GetThread() const override { return physical_frame_->GetThread(); }
  bool IsInline() const override { return true; }
  const Frame* GetPhysicalFrame() const override { return physical_frame_; }
  const Location& GetLocation() const override { return location_; }
  uint64_t GetAddress() const override { return location_.address(); }
  uint64_t GetBasePointerRegister() const override {
    return physical_frame_->GetBasePointerRegister();
  }
  std::optional<uint64_t> GetBasePointer() const override {
    return physical_frame_->GetBasePointer();
  }
  void GetBasePointerAsync(std::function<void(uint64_t bp)> cb) override {
    return physical_frame_->GetBasePointerAsync(std::move(cb));
  }
  uint64_t GetStackPointer() const override {
    return physical_frame_->GetStackPointer();
  }
  fxl::RefPtr<SymbolDataProvider> GetSymbolDataProvider() const override {
    return physical_frame_->GetSymbolDataProvider();
  }
  fxl::RefPtr<ExprEvalContext> GetExprEvalContext() const override {
    return physical_frame_->GetExprEvalContext();
  }

 private:
  Frame* physical_frame_;  // Non-owning.
  Location location_;

  FXL_DISALLOW_COPY_AND_ASSIGN(InlineFrame);
};

// Returns a fixed-up location referring to an indexed element in an inlined
// function call chain. This also handles the case where there are no inline
// calls and the function is the only one (this returns the same location).
//
// The main_location is the location returned by symbol lookup for the
// current address.
Location LocationForInlineFrameChain(
    const std::vector<const Function*>& inline_chain, size_t chain_index,
    const Location& main_location) {
  // The file/line is the call location of the next (into the future) inlined
  // function. Fall back on the file/line from the main lookup.
  const FileLine* new_line = &main_location.file_line();
  int new_column = main_location.column();
  if (chain_index > 0) {
    const Function* next_call = inline_chain[chain_index - 1];
    if (next_call->call_line().is_valid()) {
      new_line = &next_call->call_line();
      new_column = 0;  // DWARF doesn't contain inline call column.
    }
  }

  return Location(main_location.address(), *new_line, new_column,
                  main_location.symbol_context(),
                  LazySymbol(inline_chain[chain_index]));
}

}  // namespace

Stack::Stack(Delegate* delegate) : delegate_(delegate), weak_factory_(this) {}

Stack::~Stack() = default;

fxl::WeakPtr<Stack> Stack::GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

std::optional<size_t> Stack::IndexForFrame(const Frame* frame) const {
  for (size_t i = 0; i < frames_.size(); i++) {
    if (frames_[i].get() == frame)
      return i;
  }
  return std::nullopt;
}

size_t Stack::InlineDepthForIndex(size_t index) const {
  FXL_DCHECK(index < frames_.size());
  for (size_t depth = 0; index + depth < frames_.size(); depth++) {
    if (!frames_[index + depth]->IsInline())
      return depth;
  }

  FXL_NOTREACHED();  // Should have found a physical frame that generated it.
  return 0;
}

std::optional<FrameFingerprint> Stack::GetFrameFingerprint(
    size_t virtual_frame_index) const {
  size_t frame_index = virtual_frame_index + hide_top_inline_frame_count_;

  // Should reference a valid index in the array.
  if (frame_index >= frames_.size()) {
    FXL_NOTREACHED();
    return FrameFingerprint();
  }

  // The inline frame count is the number of steps from the requested frame
  // index to the current physical frame.
  size_t inline_count = InlineDepthForIndex(frame_index);

  // The stack pointer we want is the one from right before the current
  // physical frame (see frame_fingerprint.h).
  size_t before_physical_frame_index = frame_index + inline_count + 1;
  if (before_physical_frame_index == frames_.size()) {
    if (!has_all_frames())
      return std::nullopt;  // Not synchronously available.

    // For the bottom frame, this returns the frame base pointer instead which
    // will at least identify the frame in some ways, and can be used to see if
    // future frames are younger.
    return FrameFingerprint(frames_[frame_index]->GetStackPointer(), 0);
  }

  return FrameFingerprint(
      frames_[before_physical_frame_index]->GetStackPointer(), inline_count);
}

void Stack::GetFrameFingerprint(
    size_t virtual_frame_index,
    std::function<void(const Err&, FrameFingerprint)> cb) {
  size_t frame_index = virtual_frame_index + hide_top_inline_frame_count_;
  FXL_DCHECK(frame_index < frames_.size());

  // Identify the frame in question across the async call by its combination of
  // IP, SP, and inline nesting count. If anything changes we don't want to
  // issue the callback.
  uint64_t ip = frames_[frame_index]->GetAddress();
  uint64_t sp = frames_[frame_index]->GetStackPointer();
  size_t inline_count = InlineDepthForIndex(frame_index);

  // This callback is issued when the full stack is available.
  auto on_full_stack = [weak_stack = GetWeakPtr(), frame_index, ip, sp,
                        inline_count, cb = std::move(cb)](const Err& err) {
    if (err.has_error()) {
      cb(err, FrameFingerprint());
      return;
    }
    if (!weak_stack) {
      cb(Err("Thread destroyed."), FrameFingerprint());
      return;
    }
    const auto& frames = weak_stack->frames_;

    if (frame_index >= frames.size() ||
        frames[frame_index]->GetAddress() != ip ||
        frames[frame_index]->GetStackPointer() != sp ||
        weak_stack->InlineDepthForIndex(frame_index) != inline_count) {
      // Something changed about this stack item since the original call.
      // Count the request as invalid.
      cb(Err("Stack changed across queries."), FrameFingerprint());
      return;
    }

    // Should always have a fingerprint after syncing the stack.
    auto found_fingerprint = weak_stack->GetFrameFingerprint(frame_index);
    FXL_DCHECK(found_fingerprint);
    cb(Err(), *found_fingerprint);
  };

  if (has_all_frames()) {
    // All frames are available, don't force a recomputation of the stack. But
    // the caller still expects an async response. Calling the full callback
    // is important for the checking in case the frames changed while the
    // async task is pending.
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE,
        [on_full_stack = std::move(on_full_stack)]() { on_full_stack(Err()); });
  } else {
    SyncFrames(std::move(on_full_stack));
  }
}

size_t Stack::GetTopInlineFrameCount() const {
  // This can't be InlineDepthForIndex() because that takes an index relative
  // to the hide_top_inline_frame_count_ and this function always wants to
  // return the same thing regardless of the hide count.
  for (size_t i = 0; i < frames_.size(); i++) {
    if (!frames_[i]->IsInline())
      return i;
  }

  // Should always have a non-inline frame if there are any.
  FXL_DCHECK(frames_.empty());
  return 0;
}

void Stack::SetHideTopInlineFrameCount(size_t hide_count) {
  FXL_DCHECK(hide_count <= GetTopInlineFrameCount());
  hide_top_inline_frame_count_ = hide_count;
}

void Stack::SyncFrames(std::function<void(const Err&)> callback) {
  delegate_->SyncFramesForStack(std::move(callback));
}

void Stack::SetFrames(debug_ipc::ThreadRecord::StackAmount amount,
                      const std::vector<debug_ipc::StackFrame>& frames) {
  frames_.clear();
  for (const debug_ipc::StackFrame& frame : frames)
    AppendFrame(frame);
  has_all_frames_ = amount == debug_ipc::ThreadRecord::StackAmount::kFull;
}

void Stack::SetFramesForTest(std::vector<std::unique_ptr<Frame>> frames,
                             bool has_all) {
  frames_ = std::move(frames);
  has_all_frames_ = has_all;
}

bool Stack::ClearFrames() {
  has_all_frames_ = false;

  if (frames_.empty())
    return false;  // Nothing to do.

  frames_.clear();
  return true;
}

void Stack::AppendFrame(const debug_ipc::StackFrame& record) {
  // This symbolizes all stack frames since the expansion of inline frames
  // depends on the symbols. Its possible some stack objects will never have
  // their frames queried which makes this duplicate work. A possible addition
  // is to just save the debug_ipc::StackFrames and only expand the inline
  // frames when the frame list is accessed.

  // The symbols will provide the location for the innermost inlined function.
  Location inner_loc = delegate_->GetSymbolizedLocationForStackFrame(record);

  const Function* cur_func = inner_loc.symbol().Get()->AsFunction();
  if (!cur_func) {
    // No function associated with this location.
    frames_.push_back(delegate_->MakeFrameForStack(record, inner_loc));
    return;
  }

  // The Location object will reference the most-specific inline function but
  // we need the whole chain.
  std::vector<const Function*> inline_chain = cur_func->GetInlineChain();
  if (inline_chain.back()->is_inline()) {
    // A non-inline frame was not found. The symbols are corrupt so give up
    // on inline processing and add the physical frame only.
    frames_.push_back(delegate_->MakeFrameForStack(record, inner_loc));
    return;
  }

  // Need to make the base "physical" frame first because all of the inline
  // frames refer to it.
  auto physical_frame = delegate_->MakeFrameForStack(
      record, LocationForInlineFrameChain(inline_chain, inline_chain.size() - 1,
                                          inner_loc));

  // Add all inline functions (skipping the last which is the physical frame
  // made above).
  for (size_t i = 0; i < inline_chain.size() - 1; i++) {
    frames_.push_back(std::make_unique<InlineFrame>(
        physical_frame.get(),
        LocationForInlineFrameChain(inline_chain, i, inner_loc)));
  }

  // Physical frame goes last (back in time).
  frames_.push_back(std::move(physical_frame));
}

}  // namespace zxdb
