// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/system.h"

#include "garnet/bin/zxdb/client/setting_schema_definition.h"

namespace zxdb {

// Schema definition -----------------------------------------------------------

const char* ClientSettings::kSymbolPaths = "symbol-paths";
const char* kSymbolPathsDescription = R"(
      List of mapping databases, ELF files or directories for symbol lookup.
      When a directory path is passed, the directory will be enumerated
      non-recursively to index all ELF files within. When a .txt file is passed,
      it will be treated as a mapping database from build ID to file path.
      Otherwise, the path will be loaded as an ELF file.)";

namespace {

fxl::RefPtr<SettingSchema> CreateSchema() {
  auto schema = fxl::MakeRefCounted<SettingSchema>();

  schema->AddList(ClientSettings::kSymbolPaths, kSymbolPathsDescription, {});

  return schema;
}

}  // namespace

// System Implementation -------------------------------------------------------

System::System(Session* session)
    : ClientObject(session),
      settings_(SettingStore::Level::kSystem, GetSchema(), nullptr) {}
System::~System() = default;

void System::AddObserver(SystemObserver* observer) {
  observers_.AddObserver(observer);
}

void System::RemoveObserver(SystemObserver* observer) {
  observers_.RemoveObserver(observer);
}

fxl::RefPtr<SettingSchema> System::GetSchema() {
  // Will only run initialization once.
  InitializeSchemas();
  static fxl::RefPtr<SettingSchema> schema = CreateSchema();
  return schema;
}

}  // namespace zxdb
