// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/frame.h"
#include "garnet/bin/zxdb/symbols/location.h"
#include "garnet/lib/debug_ipc/records.h"
#include "lib/fxl/memory/ref_ptr.h"

namespace zxdb {

class MockSymbolDataProvider;
class SymbolEvalContext;

// Provides a MockFrame implementation that just returns constant values for
// everything. Tests can override this to implement the subset of functionality
// they need.
class MockFrame : public Frame {
 public:
  // Session and Thread can be null as long as no code that uses this object
  // needs it.
  //
  // The physical frame is the non-inlined call frame associated with this one.
  // The pointer must outlive this class (normally both are owned by the
  // Stack). A null physical frame indicates that this is not inline.
  MockFrame(Session* session, Thread* thread,
            const debug_ipc::StackFrame& stack_frame, const Location& location,
            const Frame* physical_frame = nullptr);

  ~MockFrame() override;

  const debug_ipc::StackFrame& stack_frame() const { return stack_frame_; }
  void set_stack_frame(debug_ipc::StackFrame sf) { stack_frame_ = sf; }

  // Use GetLocation() to retrieve the location.
  void set_location(Location l) { location_ = std::move(l); }

  // Frame implementation.
  Thread* GetThread() const override;
  bool IsInline() const override;
  const Frame* GetPhysicalFrame() const override;
  const Location& GetLocation() const override;
  uint64_t GetAddress() const override;
  uint64_t GetBasePointerRegister() const override;
  std::optional<uint64_t> GetBasePointer() const override;
  void GetBasePointerAsync(std::function<void(uint64_t bp)> cb) override;
  uint64_t GetStackPointer() const override;
  fxl::RefPtr<SymbolDataProvider> GetSymbolDataProvider() const override;
  fxl::RefPtr<ExprEvalContext> GetExprEvalContext() const override;

 private:
  Thread* thread_;

  debug_ipc::StackFrame stack_frame_;
  const Frame* physical_frame_;  // Null if non-inlined.
  Location location_;
  mutable fxl::RefPtr<MockSymbolDataProvider> symbol_data_provider_;  // Lazy.
  mutable fxl::RefPtr<SymbolEvalContext> symbol_eval_context_;        // Lazy.

  FXL_DISALLOW_COPY_AND_ASSIGN(MockFrame);
};

}  // namespace zxdb
