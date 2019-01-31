// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_STORAGE_IMPL_SPLIT_H_
#define PERIDOT_BIN_LEDGER_STORAGE_IMPL_SPLIT_H_

#include <lib/fit/function.h>

#include "peridot/bin/ledger/storage/impl/object_digest.h"
#include "peridot/bin/ledger/storage/public/data_source.h"
#include "peridot/bin/ledger/storage/public/types.h"

namespace storage {

// Status for the |SplitDataSource| and |CollectXXXPieces| callbacks.
enum class IterationStatus {
  DONE,
  IN_PROGRESS,
  ERROR,
};

// Splits the data from |source| representing an object of some |type| and
// builds a multi-level index from the content. The |source| is consumed and
// split using a rolling hash. Each chunk and each index file is returned. On
// each iteration, |make_object_identifier| is called first and must return the
// |ObjectIdentifier| to use to reference the given content id. This identifier
// is then passed to |callback|, along with the content itself and a status of
// |IN_PROGRESS|, except for the last chunk which has a status of |DONE|.
// |callback| is not called anymore once |source| is deleted.
void SplitDataSource(
    DataSource* source, ObjectType type,
    fit::function<ObjectIdentifier(ObjectDigest)> make_object_identifier,
    fit::function<void(IterationStatus, ObjectIdentifier,
                       std::unique_ptr<DataSource::DataChunk>)>
        callback);

// Iterates over the pieces of an index object.
Status ForEachPiece(fxl::StringView index_content,
                    fit::function<Status(ObjectIdentifier)> callback);

// Collects all pieces ids needed to build the object with id |root|. This
// returns the id of the object itself, and recurse inside any index if the
// |callback| returned true for the given id.
void CollectPieces(
    ObjectIdentifier root,
    fit::function<void(ObjectIdentifier,
                       fit::function<void(Status, fxl::StringView)>)>
        data_accessor,
    fit::function<bool(IterationStatus, ObjectIdentifier)> callback);

}  // namespace storage

#endif  // PERIDOT_BIN_LEDGER_STORAGE_IMPL_SPLIT_H_
