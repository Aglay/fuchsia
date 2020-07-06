// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidlcat/lib/inference.h"

#include <zircon/system/public/zircon/processargs.h>
#include <zircon/system/public/zircon/types.h>

#include <ios>
#include <ostream>

#include "src/lib/fidl_codec/printer.h"
#include "tools/fidlcat/lib/syscall_decoder.h"
#include "tools/fidlcat/lib/syscall_decoder_dispatcher.h"

namespace fidlcat {

void Inference::CreateHandle(zx_koid_t thread_koid, zx_handle_t handle) {
  int64_t timestamp = time(NULL);
  Thread* thread = dispatcher_->SearchThread(thread_koid);
  FX_DCHECK(thread != nullptr);
  dispatcher_->CreateHandle(thread, handle, timestamp, /*startup=*/false);
}

// This is the first function which is intercepted. This gives us information about
// all the handles an application have at startup. However, for directory handles,
// we don't have the name of the directory.
void Inference::ExtractHandles(SyscallDecoder* decoder) {
  constexpr int kNhandles = 0;
  constexpr int kHandles = 1;
  constexpr int kHandleInfo = 2;
  // Get the values which have been harvest by the debugger using they argument number.
  uint32_t nhandles = decoder->ArgumentValue(kNhandles);
  const zx_handle_t* handles =
      reinterpret_cast<const zx_handle_t*>(decoder->ArgumentContent(Stage::kEntry, kHandles));
  const uint32_t* handle_info =
      reinterpret_cast<const zx_handle_t*>(decoder->ArgumentContent(Stage::kEntry, kHandleInfo));
  int64_t timestamp = time(NULL);
  // Get the information about all the handles.
  // The meaning of handle info is described in zircon/system/public/zircon/processargs.h
  for (uint32_t handle = 0; handle < nhandles; ++handle) {
    if (handles[handle] != 0) {
      dispatcher_->CreateHandle(decoder->fidlcat_thread(), handles[handle], timestamp,
                                /*startup=*/true);
      uint32_t type = PA_HND_TYPE(handle_info[handle]);
      switch (type) {
        case PA_FD:
          AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), handles[handle], "fd",
                               PA_HND_ARG(handle_info[handle]));
          break;
        case PA_DIRECTORY_REQUEST:
          AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), handles[handle],
                               "directory-request", "/");
          break;
        default:
          AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), handles[handle], type);
          break;
      }
    }
  }
}

// This is the second function which is intercepted. This gives us information about
// all the handles which have not been used by processargs_extract_handles.
// This only adds information about directories.
void Inference::LibcExtensionsInit(SyscallDecoder* decoder) {
  constexpr int kHandleCount = 0;
  constexpr int kHandles = 1;
  constexpr int kHandleInfo = 2;
  constexpr int kNameCount = 3;
  constexpr int kNames = 4;
  // Get the values which have been harvest by the debugger using they argument number.
  uint32_t handle_count = decoder->ArgumentValue(kHandleCount);
  const zx_handle_t* handles =
      reinterpret_cast<const zx_handle_t*>(decoder->ArgumentContent(Stage::kEntry, kHandles));
  const uint32_t* handle_info =
      reinterpret_cast<const zx_handle_t*>(decoder->ArgumentContent(Stage::kEntry, kHandleInfo));
  uint32_t name_count = decoder->ArgumentValue(kNameCount);
  const uint64_t* names =
      reinterpret_cast<const uint64_t*>(decoder->ArgumentContent(Stage::kEntry, kNames));
  int64_t timestamp = time(NULL);
  // Get the information about the remaining handles.
  // The meaning of handle info is described in zircon/system/public/zircon/processargs.h
  for (uint32_t handle = 0; handle < handle_count; ++handle) {
    if (handles[handle] != 0) {
      dispatcher_->CreateHandle(decoder->fidlcat_thread(), handles[handle], timestamp,
                                /*startup=*/true);
      uint32_t type = PA_HND_TYPE(handle_info[handle]);
      switch (type) {
        case PA_NS_DIR: {
          uint32_t index = PA_HND_ARG(handle_info[handle]);
          AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), handles[handle], "dir",
                               (index < name_count)
                                   ? reinterpret_cast<const char*>(
                                         decoder->BufferContent(Stage::kEntry, names[index]))
                                   : "");
          break;
        }
        case PA_FD:
          AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), handles[handle], "fd",
                               PA_HND_ARG(handle_info[handle]));
          break;
        case PA_DIRECTORY_REQUEST:
          AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), handles[handle],
                               "directory-request", "/");
          break;
        default:
          AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), handles[handle], type);
          break;
      }
    }
  }
}

void Inference::InferMessage(SyscallDecoder* decoder,
                             fidl_codec::semantic::ContextType context_type) {
  if (decoder->semantic() == nullptr) {
    return;
  }
  constexpr int kHandle = 0;
  zx_handle_t handle = decoder->ArgumentValue(kHandle);
  if (handle != ZX_HANDLE_INVALID) {
    fidl_codec::semantic::SemanticContext context(
        this, decoder->fidlcat_thread()->process()->koid(), decoder->fidlcat_thread()->koid(),
        handle, context_type, decoder->decoded_request(), decoder->decoded_response());
    decoder->semantic()->ExecuteAssignments(&context);
  }
}

void Inference::ZxChannelCreate(SyscallDecoder* decoder) {
  constexpr int kOut0 = 1;
  constexpr int kOut1 = 2;
  zx_handle_t* out0 = reinterpret_cast<zx_handle_t*>(decoder->ArgumentContent(Stage::kExit, kOut0));
  zx_handle_t* out1 = reinterpret_cast<zx_handle_t*>(decoder->ArgumentContent(Stage::kExit, kOut1));
  if ((out0 != nullptr) && (*out0 != ZX_HANDLE_INVALID) && (out1 != nullptr) &&
      (*out1 != ZX_HANDLE_INVALID)) {
    int64_t timestamp = time(NULL);
    dispatcher_->CreateHandle(decoder->fidlcat_thread(), *out0, timestamp,
                              /*startup=*/false);
    dispatcher_->CreateHandle(decoder->fidlcat_thread(), *out1, timestamp,
                              /*startup=*/false);
    // Provides the minimal semantic for both handles (that is they are channels).
    AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), *out0, "channel",
                         next_channel_++);
    AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), *out1, "channel",
                         next_channel_++);
    // Links the two channels.
    AddLinkedHandles(decoder->fidlcat_thread()->process()->koid(), *out0, *out1);
  }
}

void Inference::ZxPortCreate(SyscallDecoder* decoder) {
  constexpr int kOut = 1;
  zx_handle_t* out = reinterpret_cast<zx_handle_t*>(decoder->ArgumentContent(Stage::kExit, kOut));
  if ((out != nullptr) && (*out != ZX_HANDLE_INVALID)) {
    int64_t timestamp = time(NULL);
    dispatcher_->CreateHandle(decoder->fidlcat_thread(), *out, timestamp,
                              /*startup=*/false);
    // Provides the minimal semantic for the handle (that is it's a port).
    AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), *out, "port", next_port_++);
  }
}

void Inference::ZxTimerCreate(SyscallDecoder* decoder) {
  constexpr int kOut = 2;
  zx_handle_t* out = reinterpret_cast<zx_handle_t*>(decoder->ArgumentContent(Stage::kExit, kOut));
  if ((out != nullptr) && (*out != ZX_HANDLE_INVALID)) {
    int64_t timestamp = time(NULL);
    dispatcher_->CreateHandle(decoder->fidlcat_thread(), *out, timestamp,
                              /*startup=*/false);
    // Provides the minimal semantic for the handle (that is it's a timer).
    AddHandleDescription(decoder->fidlcat_thread()->process()->koid(), *out, "timer",
                         next_timer_++);
  }
}

}  // namespace fidlcat
