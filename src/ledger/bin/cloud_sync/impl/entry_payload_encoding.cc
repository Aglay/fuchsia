// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/cloud_sync/impl/entry_payload_encoding.h"

#include "peridot/lib/convert/convert.h"
#include "src/ledger/bin/cloud_sync/impl/entry_payload_generated.h"
#include "src/ledger/bin/storage/public/types.h"

namespace cloud_sync {

std::string EncodeEntryPayload(const storage::Entry& entry,
                               storage::ObjectIdentifierFactory* factory) {
  flatbuffers::FlatBufferBuilder builder;
  KeyPriority priority =
      entry.priority == storage::KeyPriority::EAGER ? KeyPriority_EAGER : KeyPriority_LAZY;
  builder.Finish(CreateEntryPayload(
      builder, convert::ToFlatBufferVector(&builder, entry.key),
      convert::ToFlatBufferVector(&builder,
                                  factory->ObjectIdentifierToStorageBytes(entry.object_identifier)),
      priority));
  return std::string(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
}

bool DecodeEntryPayload(convert::ExtendedStringView entry_id, convert::ExtendedStringView payload,
                        storage::ObjectIdentifierFactory* factory, storage::Entry* entry) {
  flatbuffers::Verifier verifier(reinterpret_cast<const unsigned char*>(payload.data()),
                                 payload.size());
  if (!VerifyEntryPayloadBuffer(verifier)) {
    FXL_LOG(ERROR) << "Received invalid entry payload from the cloud.";
    return false;
  }
  const EntryPayload* entry_payload =
      GetEntryPayload(reinterpret_cast<const unsigned char*>(payload.data()));
  if (entry_payload->entry_name() == nullptr || entry_payload->object_identifier() == nullptr) {
    FXL_LOG(ERROR) << "Received invalid entry payload from the cloud.";
    return false;
  }

  entry->entry_id = entry_id.ToString();
  entry->key = convert::ToString(entry_payload->entry_name());
  entry->priority = entry_payload->priority() == KeyPriority_EAGER ? storage::KeyPriority::EAGER
                                                                   : storage::KeyPriority::LAZY;
  if (!factory->MakeObjectIdentifierFromStorageBytes(entry_payload->object_identifier(),
                                                     &entry->object_identifier)) {
    FXL_LOG(ERROR) << "Received invalid entry payload from the cloud.";
    return false;
  }

  return true;
}

}  // namespace cloud_sync
