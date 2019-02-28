// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/component/cpp/outgoing.h>
#include <lib/component/cpp/startup_context.h>
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include <test/appmgr/sandbox/cpp/fidl.h>
#include <string>

namespace {

class IsolatedStorageTestUtil
    : public test::appmgr::sandbox::DataFileReaderWriter {
 public:
  explicit IsolatedStorageTestUtil(const component::Outgoing& outgoing) {
    outgoing.AddPublicService(bindings_.GetHandler(this));
  }

  void ReadFile(std::string path, ReadFileCallback callback) override {
    std::string contents;
    if (!files::ReadFileToString(files::JoinPath("/data", path), &contents)) {
      callback(nullptr);
      return;
    }
    callback(contents);
  }

  void WriteFile(std::string path, std::string contents,
                 WriteFileCallback callback) override {
    if (!files::WriteFile(files::JoinPath("/data", path), contents.c_str(),
                          contents.length())) {
      callback(ZX_ERR_IO);
      return;
    }
    callback(ZX_OK);
  }

 private:
  fidl::BindingSet<DataFileReaderWriter> bindings_;
};

}  // namespace

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto context = component::StartupContext::CreateFromStartupInfo();
  IsolatedStorageTestUtil server(context->outgoing());
  loop.Run();
  return 0;
}
