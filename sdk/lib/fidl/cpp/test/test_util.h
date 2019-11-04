#include <lib/fidl/internal.h>

#include <iostream>

#include "gtest/gtest.h"
#include "lib/fidl/cpp/clone.h"

namespace fidl {
namespace test {
namespace util {

bool cmp_payload(const uint8_t* actual, size_t actual_size, const uint8_t* expected,
                 size_t expected_size);

template <class Output, class Input>
Output RoundTrip(const Input& input) {
  fidl::Encoder enc(0xfefefefe);

  const size_t input_encoded_size = EncodingInlineSize<Input, Encoder>(&enc);
  const size_t input_padding_size = FIDL_ALIGN(input_encoded_size) - input_encoded_size;
  const ::fidl::FidlStructField fake_input_interface_fields[] = {
      ::fidl::FidlStructField(Input::FidlType, 16, input_padding_size),
  };
  const fidl_type_t fake_input_interface_struct{
      ::fidl::FidlCodedStruct(fake_input_interface_fields, 1, 16 + input_encoded_size, "Input")};
  const size_t output_encoded_size = EncodingInlineSize<Input, Encoder>(&enc);
  const size_t output_padding_size = FIDL_ALIGN(output_encoded_size) - output_encoded_size;
  const ::fidl::FidlStructField fake_output_interface_fields[] = {
      ::fidl::FidlStructField(Output::FidlType, 16, output_padding_size),
  };
  const fidl_type_t fake_output_interface_struct{
      ::fidl::FidlCodedStruct(fake_output_interface_fields, 1, 16 + output_encoded_size, "Output")};

  auto ofs = enc.Alloc(input_encoded_size);
  fidl::Clone(input).Encode(&enc, ofs);
  auto msg = enc.GetMessage();

  const char* err_msg = nullptr;
  EXPECT_EQ(ZX_OK, msg.Validate(&fake_input_interface_struct, &err_msg)) << err_msg;
  EXPECT_EQ(ZX_OK, msg.Decode(&fake_output_interface_struct, &err_msg)) << err_msg;
  fidl::Decoder dec(std::move(msg));
  Output output;
  Output::Decode(&dec, &output, ofs);
  return output;
}

template <class Output>
Output DecodedBytes(std::vector<uint8_t> input) {
  Message message(BytePart(input.data(), input.capacity(), input.size()), HandlePart());

  fidl::FidlStructField fields_v1[] = {
      fidl::FidlStructField(Output::FidlTypeV1, sizeof(fidl_message_header_t), 0u, nullptr)};
  assert(Output::FidlTypeV1->type_tag == kFidlTypeStruct);
  fidl_type_t obj_with_header_v1 = fidl_type_t(fidl::FidlCodedStruct(
      fields_v1, 1u,
      sizeof(fidl_message_header_t) + FIDL_ALIGN(Output::FidlTypeV1->coded_struct.size), "",
      nullptr));

  fidl::FidlStructField fields_old[] = {
      fidl::FidlStructField(Output::FidlType, sizeof(fidl_message_header_t), 0u, &fields_v1[0])};
  assert(Output::FidlType->type_tag == kFidlTypeStruct);
  fidl_type_t obj_with_header_old = fidl_type_t(fidl::FidlCodedStruct(
      fields_old, 1u,
      sizeof(fidl_message_header_t) + FIDL_ALIGN(Output::FidlType->coded_struct.size), "",
      &obj_with_header_v1.coded_struct));

  const char* error = nullptr;
  EXPECT_EQ(ZX_OK, message.Decode(&obj_with_header_old, &error)) << error;

  fidl::Decoder decoder(std::move(message));
  Output output;
  Output::Decode(&decoder, &output, sizeof(fidl_message_header_t));

  return output;
}

template <class Input>
bool ValueToBytes(const Input& input, const std::vector<uint8_t>& expected) {
  fidl::Encoder enc(0xfefefefe);
  auto offset = enc.Alloc(EncodingInlineSize<Input, fidl::Encoder>(&enc));
  fidl::Clone(input).Encode(&enc, offset);
  auto msg = enc.GetMessage();
  auto payload = msg.payload();
  return cmp_payload(reinterpret_cast<const uint8_t*>(payload.data()), payload.actual(),
                     reinterpret_cast<const uint8_t*>(expected.data()), expected.size());
}

template <class Output>
void CheckDecodeFailure(std::vector<uint8_t> input, const zx_status_t expected_failure_code) {
  Message message(BytePart(input.data(), input.capacity(), input.size()), HandlePart());

  const char* error = nullptr;
  EXPECT_EQ(expected_failure_code, message.Decode(Output::FidlType, &error)) << error;
}

template <class Input>
void CheckEncodeFailure(const Input& input, const zx_status_t expected_failure_code) {
  fidl::Encoder enc(0xfefefefe);
  auto offset = enc.Alloc(EncodingInlineSize<Input, fidl::Encoder>(&enc));
  fidl::Clone(input).Encode(&enc, offset);
  auto msg = enc.GetMessage();
  const char* error = nullptr;
  EXPECT_EQ(expected_failure_code, msg.Validate(Input::FidlType, &error)) << error;
}

}  // namespace util
}  // namespace test
}  // namespace fidl
