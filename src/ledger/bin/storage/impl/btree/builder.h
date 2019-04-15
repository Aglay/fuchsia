// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_IMPL_BTREE_BUILDER_H_
#define SRC_LEDGER_BIN_STORAGE_IMPL_BTREE_BUILDER_H_

#include <lib/fit/function.h>

#include <memory>
#include <set>

#include "src/ledger/bin/storage/public/iterator.h"
#include "src/ledger/bin/storage/public/page_storage.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/ledger/lib/coroutine/coroutine.h"

namespace storage {
namespace btree {

struct NodeLevelCalculator {
  // Returns the level in the tree where a node containing |key| must be
  // located. The leaves are located on level 0.
  uint8_t (*GetNodeLevel)(convert::ExtendedStringView key);
};

// Returns the default algorithm to compute the node level.
const NodeLevelCalculator* GetDefaultNodeLevelCalculator();

// Applies changes provided by |changes| to the B-Tree starting at
// |root_identifier|. |changes| must provide |EntryChange| objects sorted by
// their key. The callback will provide the status of the operation, the id of
// the new root and the list of ids of all new nodes created after the changes.
void ApplyChanges(
    coroutine::CoroutineService* coroutine_service, PageStorage* page_storage,
    ObjectIdentifier root_identifier, std::vector<EntryChange> changes,
    fit::function<void(Status, ObjectIdentifier, std::set<ObjectIdentifier>)>
        callback,
    const NodeLevelCalculator* node_level_calculator =
        GetDefaultNodeLevelCalculator());

}  // namespace btree
}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_IMPL_BTREE_BUILDER_H_
