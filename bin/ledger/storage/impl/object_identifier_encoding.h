// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_STORAGE_IMPL_OBJECT_IDENTIFIER_ENCODING_H_
#define PERIDOT_BIN_LEDGER_STORAGE_IMPL_OBJECT_IDENTIFIER_ENCODING_H_

#include "peridot/bin/ledger/storage/impl/object_identifier_generated.h"
#include "peridot/bin/ledger/storage/public/types.h"
#include "third_party/flatbuffers/include/flatbuffers/flatbuffers.h"

namespace storage {

// Converts a |ObjectIdentifierStorage| to an |ObjectIdentifier|.
ObjectIdentifier ToObjectIdentifier(
    const ObjectIdentifierStorage* object_identifier_storage);

// Converts a |ObjectIdentifier| to an |ObjectIdentifierStorage|.
flatbuffers::Offset<ObjectIdentifierStorage> ToObjectIdentifierStorage(
    flatbuffers::FlatBufferBuilder* builder,
    const ObjectIdentifier& object_identifier);

// Encode an ObjectIdentifier into a string.
std::string EncodeObjectIdentifier(const ObjectIdentifier& object_identifier);

// Decode an ObjectIdentifier from a string. Return |true| in case of success,
// |false| otherwise.
bool DecodeObjectIdentifier(fxl::StringView data,
                            ObjectIdentifier* object_identifier);

// Returns whether a |ObjectIdentifierStorage| obtained from flatbuffer is
// valid.
bool IsObjectIdentifierStorageValid(const ObjectIdentifierStorage* storage);

}  // namespace storage

#endif  // PERIDOT_BIN_LEDGER_STORAGE_IMPL_OBJECT_IDENTIFIER_ENCODING_H_
