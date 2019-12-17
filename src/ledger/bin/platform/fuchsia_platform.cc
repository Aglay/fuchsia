
// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/platform/fuchsia_platform.h"

#include <stdio.h>

#include "src/ledger/bin/platform/fuchsia_scoped_tmp_dir.h"
#include "src/ledger/bin/platform/fuchsia_scoped_tmp_location.h"
#include "src/ledger/lib/convert/convert.h"
#include "src/ledger/lib/files/file.h"
#include "src/ledger/lib/logging/logging.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/path.h"
#include "third_party/abseil-cpp/absl/strings/string_view.h"
#include "util/env_fuchsia.h"

namespace ledger {

constexpr absl::string_view kCurrentPath = ".";

unique_fd FuchsiaFileSystem::OpenFD(DetachedPath path, DetachedPath* result_path) {
  unique_fd fd(openat(path.root_fd(), path.path().c_str(), O_RDONLY | O_DIRECTORY));
  if (fd.is_valid()) {
    *result_path = DetachedPath(fd.get());
  }
  return fd;
}

std::unique_ptr<leveldb::Env> FuchsiaFileSystem::MakeLevelDbEnvironment(
    DetachedPath db_path, DetachedPath* updated_db_path) {
  unique_fd fd;
  *updated_db_path = db_path;
  if (db_path.path() != ".") {
    // Open a FileDescriptor at the db path.
    fd = OpenFD(db_path, updated_db_path);
    if (!fd.is_valid()) {
      LEDGER_LOG(ERROR) << "Unable to open directory at " << db_path.path() << ". errno: " << errno;
      return nullptr;
    }
  }
  return leveldb::MakeFuchsiaEnv(updated_db_path->root_fd());
}

bool FuchsiaFileSystem::ReadFileToString(DetachedPath path, std::string* content) {
  return ReadFileToStringAt(path.root_fd(), path.path(), content);
}

bool FuchsiaFileSystem::WriteFile(DetachedPath path, const std::string& content) {
  return WriteFileAt(path.root_fd(), path.path(), content.c_str(), content.size());
}

bool FuchsiaFileSystem::IsFile(DetachedPath path) { return IsFileAt(path.root_fd(), path.path()); }

bool FuchsiaFileSystem::GetFileSize(DetachedPath path, uint64_t* size) {
  return GetFileSizeAt(path.root_fd(), path.path(), size);
}

bool FuchsiaFileSystem::CreateDirectory(DetachedPath path) {
  return files::CreateDirectoryAt(path.root_fd(), path.path());
}

bool FuchsiaFileSystem::IsDirectory(DetachedPath path) {
  return files::IsDirectoryAt(path.root_fd(), path.path());
}

bool FuchsiaFileSystem::GetDirectoryContents(DetachedPath path,
                                             std::vector<std::string>* dir_contents) {
  if (!files::ReadDirContentsAt(path.root_fd(), path.path(), dir_contents)) {
    return false;
  }
  // Remove the current directory string from the result.
  auto it = std::find(dir_contents->begin(), dir_contents->end(), convert::ToString(kCurrentPath));
  LEDGER_DCHECK(it != dir_contents->end());
  dir_contents->erase(it);
  return true;
}

std::unique_ptr<ScopedTmpDir> FuchsiaFileSystem::CreateScopedTmpDir(DetachedPath parent_path) {
  return std::make_unique<FuchsiaScopedTmpDir>(parent_path);
}

std::unique_ptr<ScopedTmpLocation> FuchsiaFileSystem::CreateScopedTmpLocation() {
  return std::make_unique<FuchsiaScopedTmpLocation>();
}

bool FuchsiaFileSystem::DeletePath(DetachedPath path) {
  return files::DeletePathAt(path.root_fd(), path.path(), /*recursive*/ false);
}

bool FuchsiaFileSystem::DeletePathRecursively(DetachedPath path) {
  return files::DeletePathAt(path.root_fd(), path.path(), /*recursive*/ true);
}

bool FuchsiaFileSystem::Rename(DetachedPath origin, DetachedPath destination) {
  return renameat(origin.root_fd(), origin.path().c_str(), destination.root_fd(),
                  destination.path().c_str());
}

std::unique_ptr<Platform> MakePlatform() { return std::make_unique<FuchsiaPlatform>(); }

}  // namespace ledger
