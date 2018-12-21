// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/console/format_frame.h"

#include <inttypes.h>

#include "garnet/bin/zxdb/client/frame.h"
#include "garnet/bin/zxdb/client/thread.h"
#include "garnet/bin/zxdb/console/command_utils.h"
#include "garnet/bin/zxdb/console/console.h"
#include "garnet/bin/zxdb/console/format_value.h"
#include "garnet/bin/zxdb/console/format_value_process_context_impl.h"
#include "garnet/bin/zxdb/console/output_buffer.h"
#include "garnet/bin/zxdb/console/string_util.h"
#include "garnet/bin/zxdb/symbols/function.h"
#include "garnet/bin/zxdb/symbols/location.h"
#include "garnet/bin/zxdb/symbols/value.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

void ListCompletedFrames(Thread* thread, bool include_params,
                         bool long_format) {
  Console* console = Console::get();
  int active_frame_id = console->context().GetActiveFrameIdForThread(thread);

  auto helper = fxl::MakeRefCounted<FormatValue>(
      std::make_unique<FormatValueProcessContextImpl>(thread->GetProcess()));

  // Formatting used for long format mode.
  FormatValueOptions format_options;
  format_options.verbosity = FormatValueOptions::Verbosity::kMinimal;

  // This doesn't use table output since the format of the stack frames is
  // usually so unpredictable.
  const auto& frames = thread->GetFrames();
  if (frames.empty()) {
    if (thread->GetState() != debug_ipc::ThreadRecord::State::kSuspended ||
        !(thread->GetState() == debug_ipc::ThreadRecord::State::kBlocked &&
          thread->GetBlockedReason() ==
              debug_ipc::ThreadRecord::BlockedReason::kException)) {
      // Make a nicer error message for the common case of requesting stack
      // frames when the thread is in the wrong state.
      helper->Append(
          "Stack frames are only available when the thread is either suspended "
          "or blocked\nin an exception. Use \"pause\" to suspend it.");
    } else {
      helper->Append("No stack frames.\n");
    }
  } else {
    for (int i = 0; i < static_cast<int>(frames.size()); i++) {
      if (i == active_frame_id)
        helper->Append(GetRightArrow() + " ");
      else
        helper->Append("  ");

      helper->Append(OutputBuffer::WithContents(Syntax::kSpecial,
                                                fxl::StringPrintf("%d ", i)));

      // Supply "-1" for the frame index to suppress printing (we already
      // did it above).
      if (long_format) {
        FormatFrameLong(frames[i], include_params, helper.get(),
                        format_options, -1);
      } else {
        OutputBuffer out;
        FormatFrame(frames[i], include_params, &out, -1);
        helper->Append(std::move(out));
      }

      helper->Append("\n");
    }
  }

  helper->Complete(
      [helper](OutputBuffer out) { Console::get()->Output(std::move(out)); });
}

}  // namespace

void OutputFrameList(Thread* thread, bool include_params, bool long_format) {
  // Always request an up-to-date frame list from the agent. Various things
  // could have changed and the user is manually requesting a new list, so
  // don't rely on the cached copy even if Thread::HasAllFrames() is true.
  thread->SyncFrames(
      [ thread = thread->GetWeakPtr(), include_params, long_format ]() {
        Console* console = Console::get();
        if (thread)
          ListCompletedFrames(thread.get(), include_params, long_format);
        else
          console->Output("Thread exited, no frames.\n");
      });
}

void FormatFrame(const Frame* frame, bool include_params, OutputBuffer* out,
                 int id) {
  if (id >= 0)
    out->Append(fxl::StringPrintf("Frame %d ", id));
  out->Append(FormatLocation(frame->GetLocation(), false, include_params));
}

void FormatFrameLong(const Frame* frame, bool include_params, FormatValue* out,
                     const FormatValueOptions& options, int id) {
  if (id >= 0)
    out->Append(OutputBuffer::WithContents(fxl::StringPrintf("Frame %d ", id)));

  // Only print the location if it has symbols, otherwise the hex
  // address will be shown twice.
  const Location& location = frame->GetLocation();
  if (location.has_symbols())
    out->Append(FormatLocation(location, false, include_params));

  // Long format includes the IP address.
  // TODO(brettw) handle asynchronously available BP.
  uint64_t bp = 0;
  if (auto optional_bp = frame->GetBasePointer())
    bp = *optional_bp;
  out->Append(OutputBuffer(
      Syntax::kComment,
      fxl::StringPrintf("\n      IP = 0x%" PRIx64 ", BP = 0x%" PRIx64
                        ", SP = 0x%" PRIx64,
                        frame->GetAddress(), bp, frame->GetStackPointer())));

  if (location.symbol()) {
    const Function* func = location.symbol().Get()->AsFunction();
    if (func) {
      // Always list function parameters in the order specified.
      for (const auto& param : func->parameters()) {
        const Variable* value = param.Get()->AsVariable();
        if (!value)
          continue;  // Symbols are corrupt.

        out->Append("\n      ");  // Indent.
        out->AppendVariableWithName(location.symbol_context(),
                                    frame->GetSymbolDataProvider(), value,
                                    options);
      }
    }
  }
}

}  // namespace zxdb
