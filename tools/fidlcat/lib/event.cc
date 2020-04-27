// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidlcat/lib/event.h"

#include "tools/fidlcat/lib/syscall_decoder_dispatcher.h"

namespace fidlcat {

void FidlcatPrinter::DisplayHandle(const zx_handle_info_t& handle) {
  fidl_codec::DisplayHandle(handle, *this);
  const fidl_codec::semantic::HandleDescription* known_handle =
      inference_.GetHandleDescription(process_id_, handle.handle);
  if (known_handle != nullptr) {
    (*this) << '(';
    known_handle->Display(*this);
    (*this) << ')';
  }
}

void FidlcatPrinter::DisplayStatus(zx_status_t status) {
  if (status == ZX_OK) {
    (*this) << fidl_codec::Green;
  } else {
    (*this) << fidl_codec::Red;
  }
  (*this) << fidl_codec::StatusName(status) << fidl_codec::ResetColor;
}

bool FidlcatPrinter::DisplayReturnedValue(SyscallReturnType type, int64_t returned_value) {
  switch (type) {
    case SyscallReturnType::kNoReturn:
    case SyscallReturnType::kVoid:
      return false;
    case SyscallReturnType::kStatus:
      (*this) << "-> ";
      DisplayStatus(static_cast<zx_status_t>(returned_value));
      break;
    case SyscallReturnType::kTicks:
      (*this) << "-> " << fidl_codec::Green << "ticks" << fidl_codec::ResetColor << ": "
              << fidl_codec::Blue << static_cast<uint64_t>(returned_value)
              << fidl_codec::ResetColor;
      break;
    case SyscallReturnType::kTime:
      (*this) << "-> " << fidl_codec::Green << "time" << fidl_codec::ResetColor << ": ";
      DisplayTime(static_cast<zx_time_t>(returned_value));
      break;
    case SyscallReturnType::kUint32:
      (*this) << "-> " << fidl_codec::Blue << static_cast<uint32_t>(returned_value)
              << fidl_codec::ResetColor;
      break;
    case SyscallReturnType::kUint64:
      (*this) << "-> " << fidl_codec::Blue << static_cast<uint64_t>(returned_value)
              << fidl_codec::ResetColor;
      break;
  }
  return true;
}

void FidlcatPrinter::DisplayInline(
    const std::vector<std::unique_ptr<fidl_codec::StructMember>>& members,
    const std::map<const fidl_codec::StructMember*, std::unique_ptr<fidl_codec::Value>>& values) {
  (*this) << '(';
  const char* separator = "";
  for (const auto& member : members) {
    auto it = values.find(member.get());
    if (it == values.end())
      continue;
    (*this) << separator << member->name() << ":" << fidl_codec::Green << member->type()->Name()
            << fidl_codec::ResetColor << ": ";
    it->second->PrettyPrint(member->type(), *this);
    separator = ", ";
  }
  (*this) << ")";
}

void FidlcatPrinter::DisplayOutline(
    const std::vector<std::unique_ptr<fidl_codec::StructMember>>& members,
    const std::map<const fidl_codec::StructMember*, std::unique_ptr<fidl_codec::Value>>& values) {
  fidl_codec::Indent indent(*this);
  for (const auto& member : members) {
    auto it = values.find(member.get());
    if (it == values.end())
      continue;
    auto fidl_message_value = it->second->AsFidlMessageValue();
    if (fidl_message_value != nullptr) {
      it->second->PrettyPrint(member->type(), *this);
    } else {
      (*this) << member->name() << ":" << fidl_codec::Green << member->type()->Name()
              << fidl_codec::ResetColor << ": ";
      it->second->PrettyPrint(member->type(), *this);
      (*this) << '\n';
    }
  }
}

void Process::LoadHandleInfo(Inference* inference) {
  zxdb::Process* zxdb_process = zxdb_process_.get();
  if (zxdb_process == nullptr) {
    return;
  }
  if (loading_handle_info_) {
    // We are currently loading information about the handles. If we are unlucky, the result won't
    // include information about handles we are now needing. Ask the process to do another load just
    // after the current one to be sure to have all the handles we need (including the handle only
    // needed after the start of the load).
    needs_to_load_handle_info_ = true;
    return;
  }
  loading_handle_info_ = true;
  needs_to_load_handle_info_ = false;
  zxdb_process->LoadInfoHandleTable(
      [this, inference](zxdb::ErrOr<std::vector<debug_ipc::InfoHandleExtended>> handles) {
        loading_handle_info_ = false;
        if (!handles.ok()) {
          FXL_LOG(ERROR) << "msg: " << handles.err().msg();
        } else {
          for (const auto& handle : handles.value()) {
            fidl_codec::semantic::HandleDescription* description =
                inference->GetHandleDescription(koid_, handle.handle_value);
            if (description != nullptr) {
              // Associate the koid to the handle only if the handle is currently used by the
              // monitored process. That is if the handle if referenced by an event.
              // That means that we may need an extra load if the handle is already known by the
              // kernel but not yet needed by the monitored process. This way we avoid creating
              // handle description for handle we don't know the semantic.
              description->set_koid(handle.koid);
            }
            if (handle.related_koid != ZX_HANDLE_INVALID) {
              // However, the associated of koids is always useful.
              inference->AddLinkedKoids(handle.koid, handle.related_koid);
            }
          }
          if (needs_to_load_handle_info_) {
            needs_to_load_handle_info_ = false;
            LoadHandleInfo(inference);
          }
        }
      });
}

bool Event::NeedsToLoadHandleInfo(zx_koid_t pid, Inference* inference) {
  for (const auto& field : inline_fields_) {
    if (field.second->NeedsToLoadHandleInfo(pid, inference)) {
      return true;
    }
  }
  for (const auto& field : outline_fields_) {
    if (field.second->NeedsToLoadHandleInfo(pid, inference)) {
      return true;
    }
  }
  return false;
}

void InvokedEvent::PrettyPrint(FidlcatPrinter& printer) const {
  printer << syscall()->name();
  printer.DisplayInline(syscall()->input_inline_members(), inline_fields());
  printer << '\n';
  printer.DisplayOutline(syscall()->input_outline_members(), outline_fields());
}

void OutputEvent::PrettyPrint(FidlcatPrinter& printer) const {
  fidl_codec::Indent indent(printer);
  if (!printer.DisplayReturnedValue(syscall()->return_type(), returned_value_)) {
    return;
  }
  // Adds the inline output arguments (if any).
  if (!inline_fields().empty()) {
    printer << ' ';
    printer.DisplayInline(syscall()->output_inline_members(), inline_fields());
  }
  printer << '\n';
  printer.DisplayOutline(syscall()->output_outline_members(), outline_fields());
}

}  // namespace fidlcat
