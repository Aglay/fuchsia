// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <gtest/gtest.h>

#include "src/lib/fidl_codec/library_loader.h"
#include "src/lib/fidl_codec/semantic.h"
#include "src/lib/fidl_codec/semantic_parser_test.h"
#include "src/lib/fidl_codec/wire_object.h"
#include "src/lib/fidl_codec/wire_types.h"

namespace fidl_codec {
namespace semantic {

constexpr uint64_t kPid = 0x1234;
constexpr uint32_t kHandle = 0x1111;
constexpr uint32_t kChannel0 = 0x1000;
constexpr uint32_t kChannel1 = 0x2000;
constexpr uint32_t kChannel2 = 0x3000;
constexpr uint32_t kChannel3 = 0x4000;

class BuiltinSemanticTest : public SemanticParserTest {
 public:
  BuiltinSemanticTest();

  void SetHandleSemantic(std::string_view type, std::string_view path) {
    handle_semantic_.AddHandleDescription(kPid, kHandle, type, path);
  }

  void ExecuteWrite(const MethodSemantic* method_semantic, const StructValue* request,
                    const StructValue* response);

  void ExecuteRead(const MethodSemantic* method_semantic, const StructValue* request,
                   const StructValue* response);

 protected:
  HandleSemantic handle_semantic_;
  const zx_handle_info_t channel0_;
  const zx_handle_info_t channel2_;
};

BuiltinSemanticTest::BuiltinSemanticTest()
    : channel0_({kChannel0, 0, 0, 0}), channel2_({kChannel2, 0, 0, 0}) {
  library_loader_.ParseBuiltinSemantic();
  handle_semantic_.AddLinkedHandles(kPid, kChannel0, kChannel1);
  handle_semantic_.AddLinkedHandles(kPid, kChannel2, kChannel3);
}

void BuiltinSemanticTest::ExecuteWrite(const MethodSemantic* method_semantic,
                                       const StructValue* request, const StructValue* response) {
  fidl_codec::semantic::SemanticContext context(&handle_semantic_, kPid, kHandle,
                                                ContextType::kWrite, request, response);
  method_semantic->ExecuteAssignments(&context);
}

void BuiltinSemanticTest::ExecuteRead(const MethodSemantic* method_semantic,
                                      const StructValue* request, const StructValue* response) {
  fidl_codec::semantic::SemanticContext context(&handle_semantic_, kPid, kHandle,
                                                ContextType::kRead, request, response);
  method_semantic->ExecuteAssignments(&context);
}

// Check Node::Clone: request.object = handle
TEST_F(BuiltinSemanticTest, CloneWrite) {
  // Checks that Node::Clone exists in fuchsia.io.
  Library* library = library_loader_.GetLibraryFromName("fuchsia.io");
  ASSERT_NE(library, nullptr);
  library->DecodeTypes();
  Interface* interface = nullptr;
  library->GetInterfaceByName("fuchsia.io/Node", &interface);
  ASSERT_NE(interface, nullptr);
  InterfaceMethod* method = interface->GetMethodByName("Clone");
  ASSERT_NE(method, nullptr);
  // Checks that the builtin semantic is defined for Clone.
  ASSERT_NE(method->semantic(), nullptr);

  // Check that by writing on this handle:
  SetHandleSemantic("dir", "/svc");

  // This message (we only define the fields used by the semantic):
  StructValue request(*method->request());
  request.AddField("object", std::make_unique<HandleValue>(channel0_));

  ExecuteWrite(method->semantic(), &request, nullptr);

  // We have this handle semantic for kChannel1.
  const HandleDescription* description = handle_semantic_.GetHandleDescription(kPid, kChannel1);
  ASSERT_NE(description, nullptr);
  ASSERT_EQ(description->type(), "dir");
  ASSERT_EQ(description->path(), "/svc");
}

// Check Node::Clone: request.object = handle
TEST_F(BuiltinSemanticTest, CloneRead) {
  // Checks that Node::Clone exists in fuchsia.io.
  Library* library = library_loader_.GetLibraryFromName("fuchsia.io");
  ASSERT_NE(library, nullptr);
  library->DecodeTypes();
  Interface* interface = nullptr;
  library->GetInterfaceByName("fuchsia.io/Node", &interface);
  ASSERT_NE(interface, nullptr);
  InterfaceMethod* method = interface->GetMethodByName("Clone");
  ASSERT_NE(method, nullptr);
  // Checks that the builtin semantic is defined for Clone.
  ASSERT_NE(method->semantic(), nullptr);

  // Check that by writing on this handle:
  SetHandleSemantic("dir", "/svc");

  // This message (we only define the fields used by the semantic):
  StructValue request(*method->request());
  request.AddField("object", std::make_unique<HandleValue>(channel0_));

  ExecuteRead(method->semantic(), &request, nullptr);

  // We have this handle semantic for kChannel1.
  const HandleDescription* description = handle_semantic_.GetHandleDescription(kPid, kChannel0);
  ASSERT_NE(description, nullptr);
  ASSERT_EQ(description->type(), "dir");
  ASSERT_EQ(description->path(), "/svc");
}

// Check Directory::Open: request.object = handle / request.path
TEST_F(BuiltinSemanticTest, Open) {
  // Checks that Directory::Open exists in fuchsia.io.
  Library* library = library_loader_.GetLibraryFromName("fuchsia.io");
  ASSERT_NE(library, nullptr);
  library->DecodeTypes();
  Interface* interface = nullptr;
  library->GetInterfaceByName("fuchsia.io/Directory", &interface);
  ASSERT_NE(interface, nullptr);
  InterfaceMethod* method = interface->GetMethodByName("Open");
  ASSERT_NE(method, nullptr);
  // Checks that the builtin semantic is defined for Open.
  ASSERT_NE(method->semantic(), nullptr);

  // Check that by writing on this handle:
  SetHandleSemantic("dir", "/svc");

  // This message (we only define the fields used by the semantic):
  StructValue request(*method->request());
  request.AddField("path", std::make_unique<StringValue>("fuchsia.sys.Launcher"));
  request.AddField("object", std::make_unique<HandleValue>(channel0_));

  ExecuteWrite(method->semantic(), &request, nullptr);

  // We have this handle semantic for kChannel1.
  const HandleDescription* description = handle_semantic_.GetHandleDescription(kPid, kChannel1);
  ASSERT_NE(description, nullptr);
  ASSERT_EQ(description->type(), "dir");
  ASSERT_EQ(description->path(), "/svc/fuchsia.sys.Launcher");
}

// Check Launcher::CreateComponent.
TEST_F(BuiltinSemanticTest, CreateComponent) {
  // Checks that Launcher::CreateComponent exists in fuchsia.sys.
  Library* library = library_loader_.GetLibraryFromName("fuchsia.sys");
  ASSERT_NE(library, nullptr);
  library->DecodeTypes();
  Interface* interface = nullptr;
  library->GetInterfaceByName("fuchsia.sys/Launcher", &interface);
  ASSERT_NE(interface, nullptr);
  InterfaceMethod* method = interface->GetMethodByName("CreateComponent");
  ASSERT_NE(method, nullptr);
  // Checks that the builtin semantic is defined for CreateComponent.
  ASSERT_NE(method->semantic(), nullptr);

  // Check that by writing on this handle:
  SetHandleSemantic("dir", "/svc/fuchsia.sys.Launcher");

  // This message (we only define the fields used by the semantic):
  StructValue request(*method->request());
  auto launch_info = std::make_unique<StructValue>(
      method->request()->SearchMember("launch_info")->type()->AsStructType()->struct_definition());
  launch_info->AddField("url",
                        std::make_unique<StringValue>(
                            "fuchsia-pkg://fuchsia.com/echo_server_cpp#meta/echo_server_cpp.cmx"));
  launch_info->AddField("directory_request", std::make_unique<HandleValue>(channel0_));
  request.AddField("launch_info", std::move(launch_info));
  request.AddField("controller", std::make_unique<HandleValue>(channel2_));

  ExecuteWrite(method->semantic(), &request, nullptr);

  // We have these handle semantics for kChannel1 and kChannel3.
  const HandleDescription* description_1 = handle_semantic_.GetHandleDescription(kPid, kChannel1);
  ASSERT_NE(description_1, nullptr);
  ASSERT_EQ(description_1->type(), "server");
  ASSERT_EQ(description_1->path(),
            "fuchsia-pkg://fuchsia.com/echo_server_cpp#meta/echo_server_cpp.cmx");
  const HandleDescription* description_2 = handle_semantic_.GetHandleDescription(kPid, kChannel3);
  ASSERT_NE(description_2, nullptr);
  ASSERT_EQ(description_2->type(), "server-control");
  ASSERT_EQ(description_2->path(),
            "fuchsia-pkg://fuchsia.com/echo_server_cpp#meta/echo_server_cpp.cmx");
}

}  // namespace semantic
}  // namespace fidl_codec
