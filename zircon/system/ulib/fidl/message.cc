// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl/coding.h>
#include <lib/fidl/cpp/builder.h>
#include <lib/fidl/cpp/message.h>
#include <lib/fidl/internal.h>
#include <lib/fidl/transformer.h>
#include <string.h>

#ifdef __Fuchsia__
#include <zircon/syscalls.h>
#endif

namespace fidl {

Message::Message() = default;

Message::Message(BytePart bytes, HandlePart handles)
    : bytes_(static_cast<BytePart&&>(bytes)), handles_(static_cast<HandlePart&&>(handles)) {}

Message::~Message() {
#ifdef __Fuchsia__
  if (handles_.actual() > 0) {
    zx_handle_close_many(handles_.data(), handles_.actual());
  }
#endif
  ClearHandlesUnsafe();
}

Message::Message(Message&& other)
    : bytes_(static_cast<BytePart&&>(other.bytes_)),
      handles_(static_cast<HandlePart&&>(other.handles_)) {}

Message& Message::operator=(Message&& other) {
  bytes_ = static_cast<BytePart&&>(other.bytes_);
  handles_ = static_cast<HandlePart&&>(other.handles_);
  return *this;
}

zx_status_t Message::Encode(const fidl_type_t* type, const char** error_msg_out) {
  uint32_t actual_handles = 0u;
  zx_status_t status = fidl_encode(type, bytes_.data(), bytes_.actual(), handles_.data(),
                                   handles_.capacity(), &actual_handles, error_msg_out);
  if (status == ZX_OK)
    handles_.set_actual(actual_handles);
  return status;
}

const fidl_type_t get_alt_type(const fidl_type_t* type) {
  switch (type->type_tag) {
    case kFidlTypePrimitive:
    case kFidlTypeEnum:
    case kFidlTypeBits:
    case kFidlTypeString:
    case kFidlTypeHandle:
      return *type;
    case kFidlTypeStruct:
      return fidl_type_t(*type->coded_struct.alt_type);
    case kFidlTypeUnion:
      return fidl_type_t(*type->coded_union.alt_type);
    case kFidlTypeArray:
      return fidl_type_t(*type->coded_array.alt_type);
    case kFidlTypeVector:
      return fidl_type_t(*type->coded_vector.alt_type);
    default:
      assert(false && "cannot get alt type of a type that lacks an alt type");
      return *type;
  }
}

zx_status_t Message::Decode(const fidl_type_t* type, const char** error_msg_out) {
  if (should_decode_union_from_xunion()) {
    fidl_type_t v1_type = get_alt_type(type);
    allocated_buffer.resize(ZX_CHANNEL_MAX_MSG_BYTES);
    uint32_t size;
    zx_status_t transform_status =
        fidl_transform(FIDL_TRANSFORMATION_V1_TO_OLD, &v1_type, bytes_.data(), bytes_.actual(),
                       allocated_buffer.data(), static_cast<uint32_t>(allocated_buffer.capacity()),
                       &size, error_msg_out);
    if (transform_status != ZX_OK) {
      return transform_status;
    }

    zx_status_t status = fidl_decode(type, allocated_buffer.data(), size, handles_.data(),
                                     handles_.actual(), error_msg_out);
    bytes_ = BytePart(allocated_buffer.data(), size, size);

    ClearHandlesUnsafe();
    return status;
  }

  zx_status_t status = fidl_decode(type, bytes_.data(), bytes_.actual(), handles_.data(),
                                   handles_.actual(), error_msg_out);
  ClearHandlesUnsafe();
  return status;
}

zx_status_t Message::Validate(const fidl_type_t* type, const char** error_msg_out) const {
  return fidl_validate(type, bytes_.data(), bytes_.actual(), handles_.actual(), error_msg_out);
}

#ifdef __Fuchsia__
zx_status_t Message::Read(zx_handle_t channel, uint32_t flags) {
  uint32_t actual_bytes = 0u;
  uint32_t actual_handles = 0u;
  zx_status_t status =
      zx_channel_read(channel, flags, bytes_.data(), handles_.data(), bytes_.capacity(),
                      handles_.capacity(), &actual_bytes, &actual_handles);
  if (status == ZX_OK) {
    bytes_.set_actual(actual_bytes);
    handles_.set_actual(actual_handles);
  }
  if (actual_bytes < sizeof(fidl_message_header_t)) {
    // When reading a message, the size should always greater than the header size.
    return ZX_ERR_INVALID_ARGS;
  }
  return status;
}

zx_status_t Message::Write(zx_handle_t channel, uint32_t flags) {
  zx_status_t status = zx_channel_write(channel, flags, bytes_.data(), bytes_.actual(),
                                        handles_.data(), handles_.actual());
  ClearHandlesUnsafe();
  return status;
}

zx_status_t Message::Call(zx_handle_t channel, uint32_t flags, zx_time_t deadline,
                          Message* response) {
  zx_channel_call_args_t args;
  args.wr_bytes = bytes_.data();
  args.wr_handles = handles_.data();
  args.rd_bytes = response->bytes_.data();
  args.rd_handles = response->handles_.data();
  args.wr_num_bytes = bytes_.actual();
  args.wr_num_handles = handles_.actual();
  args.rd_num_bytes = response->bytes_.capacity();
  args.rd_num_handles = response->handles_.capacity();
  uint32_t actual_bytes = 0u;
  uint32_t actual_handles = 0u;
  zx_status_t status =
      zx_channel_call(channel, flags, deadline, &args, &actual_bytes, &actual_handles);
  ClearHandlesUnsafe();
  if (status == ZX_OK) {
    response->bytes_.set_actual(actual_bytes);
    response->handles_.set_actual(actual_handles);
  }
  return status;
}
#endif

void Message::ClearHandlesUnsafe() { handles_.set_actual(0u); }

}  // namespace fidl
