// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_IMPL_DB_SERIALIZATION_H_
#define SRC_LEDGER_BIN_STORAGE_IMPL_DB_SERIALIZATION_H_

#include <lib/fxl/strings/string_view.h>

#include "peridot/lib/rng/random.h"
#include "src/ledger/bin/storage/impl/page_db.h"
#include "src/ledger/bin/storage/public/types.h"

namespace storage {

class HeadRow {
 public:
  static constexpr fxl::StringView kPrefix = "heads/";

  static std::string GetKeyFor(CommitIdView head);
};

class MergeRow {
 public:
  static constexpr fxl::StringView kPrefix = "merges/";

  static std::string GetKeyFor(CommitIdView merge_commit_id,
                               CommitIdView parent1_id,
                               CommitIdView parent2_id);
  static std::string GetEntriesPrefixFor(CommitIdView parent1_id,
                                         CommitIdView parent2_id);
};

class CommitRow {
 public:
  static constexpr fxl::StringView kPrefix = "commits/";

  static std::string GetKeyFor(CommitIdView commit_id);
};

class ObjectRow {
 public:
  static constexpr fxl::StringView kPrefix = "objects/";

  static std::string GetKeyFor(const ObjectDigest& object_digest);
};

class UnsyncedCommitRow {
 public:
  static constexpr fxl::StringView kPrefix = "unsynced/commits/";

  static std::string GetKeyFor(const CommitId& commit_id);
};

class ObjectStatusRow {
 public:
  static constexpr fxl::StringView kTransientPrefix =
      "transient/object_digests/";
  static constexpr fxl::StringView kLocalPrefix = "local/object_digests/";
  static constexpr fxl::StringView kSyncedPrefix = "synced/object_digests/";

  static std::string GetKeyFor(PageDbObjectStatus object_status,
                               const ObjectIdentifier& object_identifier);

 private:
  static fxl::StringView GetPrefixFor(PageDbObjectStatus object_status);
};

class SyncMetadataRow {
 public:
  static constexpr fxl::StringView kPrefix = "sync_metadata/";

  static std::string GetKeyFor(fxl::StringView key);
};

class PageIsOnlineRow {
 public:
  static constexpr fxl::StringView kKey = "page_is_online";
};

}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_IMPL_DB_SERIALIZATION_H_
