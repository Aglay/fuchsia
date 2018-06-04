// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_GRPC_STATUS_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_GRPC_STATUS_H_

#include <grpc++/grpc++.h>

#include <fuchsia/ledger/cloud/cpp/fidl.h>

#include "peridot/bin/cloud_provider_firestore/include/types.h"

namespace cloud_provider_firestore {

cloud_provider::Status ConvertGrpcStatus(grpc::StatusCode status);

// If |status| is not OK, logs an error and returns true. Otherwise, returns
// false.
bool LogGrpcRequestError(const grpc::Status& status);

// If |status| is not OK, logs an error and returns true. Otherwise, returns
// false.
bool LogGrpcConnectionError(const grpc::Status& status);

}  // namespace cloud_provider_firestore

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_GRPC_STATUS_H_
