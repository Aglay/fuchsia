// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wire_parser.h"

#include <lib/async-loop/cpp/loop.h>

#include <iostream>
#include <string>
#include <vector>

#include "garnet/bin/fidlcat/lib/library_loader_test_data.h"
#include "gtest/gtest.h"
#include "lib/fidl/cpp/test/frobinator_impl.h"
#include "library_loader.h"
#include "test/fidlcat/examples/cpp/fidl.h"

// TB deleted
#include <rapidjson/writer.h>
#include "rapidjson/stringbuffer.h"

namespace fidlcat {

// Stolen from //garnet/public/lib/fidl/cpp/test/async_loop_for_test.{h,cc}; cc
// is not public

class AsyncLoopForTestImpl;

class AsyncLoopForTest {
 public:
  // The AsyncLoopForTest constructor should also call
  // async_set_default_dispatcher() with the chosen dispatcher implementation.
  AsyncLoopForTest();
  ~AsyncLoopForTest();

  // This call matches the behavior of async_loop_run_until_idle().
  zx_status_t RunUntilIdle();

  // This call matches the behavior of async_loop_run().
  zx_status_t Run();

  // Returns the underlying async_t.
  async_dispatcher_t* dispatcher();

 private:
  std::unique_ptr<AsyncLoopForTestImpl> impl_;
};

class AsyncLoopForTestImpl {
 public:
  AsyncLoopForTestImpl() : loop_(&kAsyncLoopConfigAttachToThread) {}
  ~AsyncLoopForTestImpl() = default;

  async::Loop* loop() { return &loop_; }

 private:
  async::Loop loop_;
};

AsyncLoopForTest::AsyncLoopForTest()
    : impl_(std::make_unique<AsyncLoopForTestImpl>()) {}

AsyncLoopForTest::~AsyncLoopForTest() = default;

zx_status_t AsyncLoopForTest::RunUntilIdle() {
  return impl_->loop()->RunUntilIdle();
}

zx_status_t AsyncLoopForTest::Run() { return impl_->loop()->Run(); }

async_dispatcher_t* AsyncLoopForTest::dispatcher() {
  return impl_->loop()->dispatcher();
}

LibraryLoader* InitLoader() {
  std::vector<std::unique_ptr<std::istream>> library_files;
  fidlcat_test::ExampleMap examples;
  for (auto element : examples.map()) {
    std::unique_ptr<std::istream> file = std::make_unique<std::istringstream>(
        std::istringstream(element.second));
    library_files.push_back(std::move(file));
  }
  LibraryReadError err;
  return new LibraryLoader(library_files, &err);
}

LibraryLoader* GetLoader() {
  static LibraryLoader* loader = InitLoader();
  return loader;
}

class WireParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loader_ = GetLoader();
    ASSERT_NE(loader_, nullptr);
  };
  LibraryLoader* loader_;
};

// The tests in this file work the following way:
// 1) Create a channel.
// 2) Bind an interface pointer to the client side of that channel.
// 3) Listen at the other end of the channel for the message.
// 4) Convert the message to JSON using the JSON message converter, and check
//    that the results look as expected.

// This binds |invoke| to one end of a channel, invokes it, and drops the wire
// format bits it picks up off the other end into |message|.
template <class T>
void InterceptRequest(fidl::Message& message,
                      std::function<void(fidl::InterfacePtr<T>&)> invoke) {
  AsyncLoopForTest loop;

  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));
  fidl::InterfacePtr<T> ptr;
  int error_count = 0;
  ptr.set_error_handler([&error_count](zx_status_t status) {
    EXPECT_EQ(ZX_ERR_INVALID_ARGS, status);
    ++error_count;
  });

  EXPECT_EQ(ZX_OK, ptr.Bind(std::move(h1)));

  invoke(ptr);

  loop.RunUntilIdle();

  EXPECT_EQ(ZX_OK, message.Read(h2.get(), 0));
}

TEST_F(WireParserTest, ParseSingleString) {
  fidl::MessageBuffer buffer;
  fidl::Message message = buffer.CreateEmptyMessage();

  InterceptRequest<fidl::test::frobinator::Frobinator>(
      message, [](fidl::InterfacePtr<fidl::test::frobinator::Frobinator>& ptr) {
        ptr->Grob("one", [](fidl::StringPtr value) { FAIL(); });
      });

  fidl_message_header_t header = message.header();

  const InterfaceMethod* method;
  ASSERT_TRUE(loader_->GetByOrdinal(header.ordinal, &method));
  ASSERT_STREQ("Grob", method->name().c_str());
  rapidjson::Document actual;
  fidlcat::RequestToJSON(method, message, actual);

  rapidjson::Document expected;
  expected.Parse<rapidjson::kParseNumbersAsStringsFlag>(
      R"JSON({"value":"one"})JSON");
  ASSERT_EQ(expected, actual);
}

// This is a general-purpose macro for calling InterceptRequest and checking its
// results.  It can be generalized to a wide variety of types (and is, below).
// _testname is the name of the test (prepended by Parse in the output)
// _iface is the interface method name on examples::this_is_an_interface
//    (TODO: generalize which interface to use)
// _fn is the function to use to generate the golden value for the JSON
//     output. _key and _value will be passed to it as parameters. It must
//     return a std::string.  See TwoTuple below for an example of how you can
//     have multiple keys and values.
// _key is the JSON key, which is usually going to be one or more identifiers of
//     an interface method parameter.
// _value is the JSON value, which is going to be passed to the interface.  It
//     is usually going to be a legal value for the corresponding key.
#define TEST_WIRE_TO_JSON(_testname, _iface, _fn, _key, _value)             \
  TEST_F(WireParserTest, Parse##_testname) {                                \
    fidl::MessageBuffer buffer;                                             \
    fidl::Message message = buffer.CreateEmptyMessage();                    \
    using test::fidlcat::examples::this_is_an_interface;                    \
    InterceptRequest<this_is_an_interface>(                                 \
        message, [](fidl::InterfacePtr<this_is_an_interface>& ptr) {        \
          ptr->_iface(_value);                                              \
        });                                                                 \
                                                                            \
    fidl_message_header_t header = message.header();                        \
                                                                            \
    const InterfaceMethod* method;                                          \
    ASSERT_TRUE(loader_->GetByOrdinal(header.ordinal, &method));            \
    ASSERT_STREQ(#_iface, method->name().c_str());                          \
    rapidjson::Document actual;                                             \
    fidlcat::RequestToJSON(method, message, actual);                        \
    rapidjson::StringBuffer actual_string;                                  \
    rapidjson::Writer<rapidjson::StringBuffer> actual_w(actual_string);     \
    actual.Accept(actual_w);                                                \
                                                                            \
    rapidjson::Document expected;                                           \
    std::string expected_source = _fn(_key, _value);                        \
    expected.Parse<rapidjson::kParseNumbersAsStringsFlag>(                  \
        expected_source.c_str());                                           \
    rapidjson::StringBuffer expected_string;                                \
    rapidjson::Writer<rapidjson::StringBuffer> expected_w(expected_string); \
    expected.Accept(expected_w);                                            \
                                                                            \
    ASSERT_EQ(expected, actual)                                             \
        << "expected = " << expected_string.GetString() << " ("             \
        << expected_source << ")"                                           \
        << " and actual = " << actual_string.GetString();                   \
  }

std::string RawPair(std::string key, std::string value) {
  std::string result = "{\"";
  result.append(key);
  result.append("\":");
  result.append(value);
  result.append("}");
  return result;
}

// Scalar Tests

template <class T>
std::string SingleToJson(std::string key, T value) {
  return RawPair(key, "\"" + std::to_string(value) + "\"");
}

#define TEST_SINGLE(_iface, _value, _key) \
  TEST_WIRE_TO_JSON(_iface, _iface, SingleToJson, #_key, _value)

TEST_SINGLE(Float32, 0.25, f32)
TEST_SINGLE(Float64, 9007199254740992.0, f64)
TEST_SINGLE(Int8, std::numeric_limits<int8_t>::min(), i8)
TEST_SINGLE(Int16, std::numeric_limits<int16_t>::min(), i16)
TEST_SINGLE(Int32, std::numeric_limits<int32_t>::min(), i32)
TEST_SINGLE(Int64, std::numeric_limits<int64_t>::min(), i64)
TEST_SINGLE(Uint8, std::numeric_limits<uint8_t>::max(), i8)
TEST_SINGLE(Uint16, std::numeric_limits<uint16_t>::max(), i16)
TEST_SINGLE(Uint32, std::numeric_limits<uint32_t>::max(), i32)
TEST_SINGLE(Uint64, std::numeric_limits<uint64_t>::max(), i64)

std::string BoolToJson(std::string key, bool value) {
  std::string result = "{\"";
  result.append(key);
  result.append("\":\"");
  result.append(value ? "true" : "false");
  result.append("\"}");
  return result;
}

TEST_WIRE_TO_JSON(SingleBool, Bool, BoolToJson, "b", true)

std::string ComplexToJson(std::string key1, std::string key2, int value1,
                          int value2) {
  std::ostringstream ret;
  ret << "{\"";
  ret << key1;
  ret << "\":\"";
  ret << value1;
  ret << "\",\"";
  ret << key2;
  ret << "\":\"";
  ret << value2;
  ret << "\"}";
  return ret.str();
}

#define COMMA ,

TEST_WIRE_TO_JSON(TwoTuple, Complex, ComplexToJson, "real" COMMA "imaginary",
                  1 COMMA 2)

// Vector / Array Tests

namespace {
int32_t one_param[1] = {1};
int32_t two_params[2] = {1, 2};

template <class T, size_t N>
fidl::Array<T, N> ToArray(T ts[N]) {
  ::fidl::Array<T, N> ret;
  std::copy_n(&ts[0], N, ret.begin());
  return ret;
}

// Converts an array to a JSON array, so that it can be compared against the
// results generated by the JSON parser.
template <class T, size_t N>
std::string ArrayToJsonArray(std::string param, fidl::Array<T, N> ts) {
  std::ostringstream ret;
  ret << "{\"";
  ret << param;
  ret << "\":";
  ret << "[";
  ;
  for (size_t i = 0; i < N; i++) {
    ret << "\"";
    ret << ts[i];
    ret << "\"";
    if (i != N - 1)
      ret << ",";
  }
  ret << "]}";
  return ret.str();
}

// Converts an array to a VectorPtr, so that it can be passed to a FIDL
// interface.
template <class T>
fidl::VectorPtr<T> ToVector(T ts[], size_t n) {
  std::vector<T> vec;
  for (size_t i = 0; i < n; i++) {
    vec.push_back(ts[i]);
  }
  ::fidl::VectorPtr<T> ret(vec);
  return ret;
}

// Converts an array to a JSON array, so that it can be compared against the
// results generated by the JSON parser.
template <class T>
std::string VectorToJsonArray(std::string param, fidl::VectorPtr<T> ts) {
  std::ostringstream ret;
  ret << "{\"";
  ret << param;
  ret << "\":";
  ret << "[";
  size_t sz = ts.get().size();
  for (size_t i = 0; i < sz; i++) {
    ret << "\"";
    ret << ts.get()[i];
    ret << "\"";
    if (i != sz - 1)
      ret << ",";
  }
  ret << "]}";
  return ret.str();
}

}  // namespace

#define COMMA ,

TEST_WIRE_TO_JSON(Array1, Array1, ArrayToJsonArray<int32_t COMMA 1>, "b_1",
                  (ToArray<int32_t, 1>(one_param)));
TEST_WIRE_TO_JSON(Array2, Array2, ArrayToJsonArray<int32_t COMMA 2>, "b_2",
                  (ToArray<int32_t, 2>(two_params)));
TEST_WIRE_TO_JSON(VectorOneElt, Vector, VectorToJsonArray<int32_t>, "v_1",
                  (ToVector<int32_t>(one_param, 1)));

std::string NullPair(std::string key, void* v) { return RawPair(key, "null"); }

TEST_WIRE_TO_JSON(NullVector, Vector, NullPair, "v_1", nullptr)

// Struct Tests

TEST_F(WireParserTest, ParseSingleStruct) {
  fidl::MessageBuffer buffer;
  fidl::Message message = buffer.CreateEmptyMessage();

  test::fidlcat::examples::primitive_types pt;
  pt.b = true;
  pt.i8 = std::numeric_limits<int8_t>::min();
  pt.i16 = std::numeric_limits<int16_t>::min();
  pt.i32 = std::numeric_limits<int32_t>::min();
  pt.i64 = std::numeric_limits<int64_t>::min();
  pt.u8 = std::numeric_limits<uint8_t>::max();
  pt.u16 = std::numeric_limits<uint16_t>::max();
  pt.u32 = std::numeric_limits<uint32_t>::max();
  pt.u64 = std::numeric_limits<uint64_t>::max();
  pt.f32 = 0.25;
  pt.f64 = 9007199254740992.0;

  InterceptRequest<test::fidlcat::examples::this_is_an_interface>(
      message,
      [pt](fidl::InterfacePtr<test::fidlcat::examples::this_is_an_interface>&
               ptr) { ptr->Struct(pt); });
  fidl_message_header_t header = message.header();
  const InterfaceMethod* method;
  ASSERT_TRUE(loader_->GetByOrdinal(header.ordinal, &method));
  ASSERT_STREQ("Struct", method->name().c_str());
  rapidjson::Document actual;
  fidlcat::RequestToJSON(method, message, actual);

  rapidjson::Document expected;
  std::ostringstream es;
  es << R"JSON({"p":{)JSON"
     << R"JSON("b":"true",)JSON"
     << R"JSON("i8":")JSON"
     << std::to_string(std::numeric_limits<int8_t>::min())
     << R"JSON(", "i16":")JSON"
     << std::to_string(std::numeric_limits<int16_t>::min())
     << R"JSON(", "i32":")JSON"
     << std::to_string(std::numeric_limits<int32_t>::min())
     << R"JSON(", "i64":")JSON"
     << std::to_string(std::numeric_limits<int64_t>::min())
     << R"JSON(", "u8":")JSON"
     << std::to_string(std::numeric_limits<uint8_t>::max())
     << R"JSON(", "u16":")JSON"
     << std::to_string(std::numeric_limits<uint16_t>::max())
     << R"JSON(", "u32":")JSON"
     << std::to_string(std::numeric_limits<uint32_t>::max())
     << R"JSON(", "u64":")JSON"
     << std::to_string(std::numeric_limits<uint64_t>::max())
     << R"JSON(", "f32":")JSON" << std::to_string(0.25)
     << R"JSON(", "f64":")JSON" << std::to_string(9007199254740992.0) << "\"}}";
  expected.Parse<rapidjson::kParseNumbersAsStringsFlag>(es.str().c_str());
  ASSERT_TRUE(expected == actual);
}

// Enum Tests

template <class T>
std::string XPair(std::string key, T v) {
  return RawPair(key, "\"x\"");
}

TEST_WIRE_TO_JSON(DefaultEnum, DefaultEnum,
                  XPair<test::fidlcat::examples::default_enum>, "ev",
                  test::fidlcat::examples::default_enum::x);
TEST_WIRE_TO_JSON(I8Enum, I8Enum, XPair<test::fidlcat::examples::i8_enum>, "ev",
                  test::fidlcat::examples::i8_enum::x);
TEST_WIRE_TO_JSON(I16Enum, I16Enum, XPair<test::fidlcat::examples::i16_enum>,
                  "ev", test::fidlcat::examples::i16_enum::x);

}  // namespace fidlcat
