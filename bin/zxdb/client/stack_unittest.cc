// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>

#include "garnet/bin/zxdb/client/frame_fingerprint.h"
#include "garnet/bin/zxdb/client/mock_frame.h"
#include "garnet/bin/zxdb/client/stack.h"
#include "garnet/bin/zxdb/symbols/function.h"
#include "gtest/gtest.h"
#include "lib/fxl/logging.h"

namespace zxdb {

namespace {

class MockStackDelegate : public Stack::Delegate {
 public:
  // Adds the given location to the list of things returned by
  // GetSymbolizedLocationForStackFrame().
  void AddLocation(const Location& loc) { locations_[loc.address()] = loc; }

  void SyncFramesForStack(std::function<void()> callback) override {
    // Not needed by this test.
    FXL_NOTREACHED();
  }

  std::unique_ptr<Frame> MakeFrameForStack(const debug_ipc::StackFrame& input,
                                           Location location) override {
    return std::make_unique<MockFrame>(nullptr, nullptr, input, location);
  }

  Location GetSymbolizedLocationForStackFrame(
      const debug_ipc::StackFrame& input) override {
    auto found = locations_.find(input.ip);
    if (found == locations_.end())
      return Location(Location::State::kSymbolized, input.ip);
    return found->second;
  }

 private:
  std::map<uint64_t, Location> locations_;
};

}  // namespace

// Tests fingerprint computations involving inline frames.
TEST(Stack, InlineFingerprint) {
  constexpr uint64_t kTopSP = 0x2000;
  constexpr uint64_t kMiddleSP = 0x2020;
  constexpr uint64_t kBottomSP = 0x2040;

  // Create two physical frames.
  debug_ipc::StackFrame phys_top_record(0x1000, kTopSP, kTopSP);
  Location top_location(Location::State::kSymbolized, phys_top_record.ip);
  debug_ipc::StackFrame phys_middle_record(0x1010, kMiddleSP, kMiddleSP);
  Location middle_location(Location::State::kSymbolized, phys_middle_record.ip);
  debug_ipc::StackFrame phys_bottom_record(0x1020, kBottomSP, kBottomSP);
  Location bottom_location(Location::State::kSymbolized, phys_bottom_record.ip);

  auto phys_top = std::make_unique<MockFrame>(nullptr, nullptr, phys_top_record,
                                              top_location);
  auto phys_middle = std::make_unique<MockFrame>(
      nullptr, nullptr, phys_middle_record, middle_location);
  auto phys_bottom = std::make_unique<MockFrame>(
      nullptr, nullptr, phys_bottom_record, bottom_location);

  std::vector<std::unique_ptr<Frame>> frames;

  // Top frame has two inline functions expanded on top of it. This uses the
  // same Location object for simplicity, in real life these will be different.
  frames.push_back(std::make_unique<MockFrame>(
      nullptr, nullptr, phys_top_record, top_location, phys_top.get()));
  frames.push_back(std::make_unique<MockFrame>(
      nullptr, nullptr, phys_top_record, top_location, phys_top.get()));

  // Physical top frame below those.
  frames.push_back(std::move(phys_top));

  // Middle frame has one inline function expanded on top of it.
  frames.push_back(std::make_unique<MockFrame>(
      nullptr, nullptr, phys_middle_record, middle_location, phys_middle.get()));
  frames.push_back(std::move(phys_middle));

  // Bottom frame has no inline frame.
  frames.push_back(std::move(phys_bottom));

  MockStackDelegate delegate;
  Stack stack(&delegate);
  stack.SetFramesForTest(std::move(frames), true);

  // The top frames (physical and inline) have the middle frame's SP as their
  // fingerprint, along with the inline count.
  EXPECT_EQ(FrameFingerprint(kMiddleSP, 2), stack.GetFrameFingerprint(0));
  EXPECT_EQ(FrameFingerprint(kMiddleSP, 1), stack.GetFrameFingerprint(1));
  EXPECT_EQ(FrameFingerprint(kMiddleSP, 0), stack.GetFrameFingerprint(2));

  // Middle frames have the bottom frame's SP.
  EXPECT_EQ(FrameFingerprint(kBottomSP, 1), stack.GetFrameFingerprint(3));
  EXPECT_EQ(FrameFingerprint(kBottomSP, 0), stack.GetFrameFingerprint(4));

  // Since there's nothing below the bottom frame, it gets its own SP.
  EXPECT_EQ(FrameFingerprint(kBottomSP, 0), stack.GetFrameFingerprint(5));
}

// Tests that stack frames inside inline functions are expanded so that the
// inline functions have their own "inline" frames.
TEST(Stack, InlineExpansion) {
  constexpr uint64_t kBottomAddr = 0x127365;  // IP for bottom stack frame.
  constexpr uint64_t kTopAddr = 0x893746123;  // IP for top stack frale.

  const char kFileName[] = "file.cc";
  FileLine inline_call_line(kFileName, 10);
  FileLine inline_exec_line(kFileName, 20);
  FileLine top_line(kFileName, 30);

  MockStackDelegate delegate;
  SymbolContext symbol_context = SymbolContext::ForRelativeAddresses();

  // Non-inline location for the top stack frame.
  auto top_func = fxl::MakeRefCounted<Function>(Symbol::kTagSubprogram);
  top_func->set_assigned_name("Top");
  Location top_location(kTopAddr, top_line, 0, symbol_context,
                        LazySymbol(top_func));
  delegate.AddLocation(top_location);

  // Bottom stack frame has a real function and an inline function.
  auto bottom_inline_func =
      fxl::MakeRefCounted<Function>(Symbol::kTagInlinedSubroutine);
  bottom_inline_func->set_assigned_name("Inline");
  bottom_inline_func->set_code_ranges(
      AddressRanges(AddressRange(kBottomAddr, kBottomAddr + 8)));
  bottom_inline_func->set_call_line(inline_call_line);

  auto bottom_func = fxl::MakeRefCounted<Function>(Symbol::kTagSubprogram);
  bottom_func->set_assigned_name("Bottom");
  bottom_func->set_code_ranges(
      AddressRanges(AddressRange(kBottomAddr - 8, kBottomAddr + 16)));

  // For convenience, the inline function is nested inside the "bottom" func.
  // This is not something you can actually do in C++ and will give a name
  // "Bottom::Inline()". In real life the inline function will reference the
  // actualy function definition in the correct namespace.
  bottom_inline_func->set_parent(LazySymbol(bottom_func));

  // The location returned by the symbol function will have the file/line
  // inside the inline function.
  Location bottom_location(kBottomAddr, inline_exec_line, 0, symbol_context,
                           LazySymbol(bottom_inline_func));
  delegate.AddLocation(bottom_location);

  Stack stack(&delegate);

  // Send IPs that will map to the bottom and top addresses.
  stack.SetFrames(debug_ipc::ThreadRecord::StackAmount::kFull,
                  {debug_ipc::StackFrame(kTopAddr, 0x100, 0x100),
                   debug_ipc::StackFrame(kBottomAddr, 0x200, 0x200)});

  // This should expand to tree stack entries, the one in the middle should
  // be the inline function expanded from the "bottom".
  EXPECT_EQ(3u, stack.size());

  // Bottom stack frame should be the non-inline bottom function.
  EXPECT_FALSE(stack[2]->IsInline());
  EXPECT_EQ(stack[2], stack[2]->GetPhysicalFrame());
  EXPECT_EQ(kBottomAddr, stack[2]->GetAddress());
  Location loc = stack[2]->GetLocation();
  EXPECT_EQ(kBottomAddr, loc.address());
  EXPECT_EQ(inline_call_line, loc.file_line());
  EXPECT_EQ(bottom_func.get(), loc.symbol().Get()->AsFunction());

  // Middle stack frame should be the inline bottom function at the same
  // address, referencing the bottom one as the physical frame.
  EXPECT_TRUE(stack[1]->IsInline());
  EXPECT_EQ(stack[2], stack[1]->GetPhysicalFrame());
  EXPECT_EQ(kBottomAddr, stack[1]->GetAddress());
  loc = stack[1]->GetLocation();
  EXPECT_EQ(kBottomAddr, loc.address());
  EXPECT_EQ(inline_exec_line, loc.file_line());
  EXPECT_EQ(bottom_inline_func.get(), loc.symbol().Get()->AsFunction());

  // Top stack frame.
  EXPECT_FALSE(stack[0]->IsInline());
  EXPECT_EQ(stack[0], stack[0]->GetPhysicalFrame());
  EXPECT_EQ(kTopAddr, stack[0]->GetAddress());
  loc = stack[0]->GetLocation();
  EXPECT_EQ(kTopAddr, loc.address());
  EXPECT_EQ(top_line, loc.file_line());
  EXPECT_EQ(top_func.get(), loc.symbol().Get()->AsFunction());
}

TEST(Stack, InlineHiding) {
  constexpr uint64_t kTopSP = 0x2000;
  constexpr uint64_t kBottomSP = 0x2020;

  // Create two physical frames.
  debug_ipc::StackFrame phys_top_record(0x1000, kTopSP, kTopSP);
  Location top_location(Location::State::kSymbolized, phys_top_record.ip);
  debug_ipc::StackFrame phys_bottom_record(0x1020, kBottomSP, kBottomSP);
  Location bottom_location(Location::State::kSymbolized, phys_bottom_record.ip);

  auto phys_top = std::make_unique<MockFrame>(nullptr, nullptr, phys_top_record,
                                              top_location);
  auto phys_bottom = std::make_unique<MockFrame>(
      nullptr, nullptr, phys_bottom_record, bottom_location);

  std::vector<std::unique_ptr<Frame>> frames;

  // Top frame has two inline functions expanded on top of it.
  frames.push_back(std::make_unique<MockFrame>(
      nullptr, nullptr, phys_top_record, top_location, phys_top.get()));
  frames.push_back(std::make_unique<MockFrame>(
      nullptr, nullptr, phys_top_record, top_location, phys_top.get()));

  // Physical top frame below those.
  frames.push_back(std::move(phys_top));

  // Bottom frame has no inline frame.
  frames.push_back(std::move(phys_bottom));

  MockStackDelegate delegate;
  Stack stack(&delegate);

  // With no frames, there should be no inline frames.
  EXPECT_EQ(0u, stack.GetTopInlineFrameCount());

  // Setting the frames should give the two inline ones, followed by two
  // physical ones.
  stack.SetFramesForTest(std::move(frames), true);
  EXPECT_EQ(4u, stack.size());
  EXPECT_EQ(2u, stack.GetTopInlineFrameCount());

  // Hide both inline frames, the top frame should now be the physical one.
  stack.SetHideTopInlineFrameCount(2);
  EXPECT_EQ(2u, stack.size());
  EXPECT_EQ(2u, stack.GetTopInlineFrameCount());
}

}  // namespace zxdb
