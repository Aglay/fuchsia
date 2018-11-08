// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_STORAGE_IMPL_LEVELDB_FACTORY_H_
#define PERIDOT_BIN_LEDGER_STORAGE_IMPL_LEVELDB_FACTORY_H_

#include "peridot/bin/ledger/storage/public/db_factory.h"

#include <memory>

#include <lib/fxl/memory/weak_ptr.h>

#include "peridot/bin/ledger/coroutine/coroutine_manager.h"
#include "peridot/bin/ledger/environment/environment.h"
#include "peridot/bin/ledger/filesystem/detached_path.h"
#include "peridot/bin/ledger/storage/impl/leveldb.h"

namespace storage {

// A factory for LevelDb instances.
//
// This factory tries to always keep a new empty instance of LevelDb,
// initialized and pre-cached, in order to immediately respond to requests for
// new Db instances.
//
// When creating new LevelDb instances, using |GetOrCreateDb|, the caller should
// make sure that there is no live LevelDb instance for the same path.
class LevelDbFactory : public DbFactory {
 public:
  LevelDbFactory(ledger::Environment* environment,
                 ledger::DetachedPath cache_path);

  // Initializes the LevelDbFactory by preparing the cached instance of LevelDb.
  void Init();

  // DbFactory:
  void GetOrCreateDb(
      ledger::DetachedPath db_path,
      fit::function<void(Status, std::unique_ptr<Db>)> callback) override;

 private:
  struct DbInitializationState;
  enum class CreateInStagingPath : bool;

  // Gets or creates a new LevelDb instance in the given |db_path|,
  // initializes it in the I/O thread and then returns it through the
  // |callback|.
  void GetOrCreateDbAtPath(
      ledger::DetachedPath db_path, CreateInStagingPath create_in_staging_path,
      fit::function<void(Status, std::unique_ptr<Db>)> callback);

  // Gets or creates a new LevelDb instance. This method should be
  // called from the I/O thread. When initialization is complete, it makes sure
  // to call the |callback| with the computed result from the main thread.
  void GetOrCreateDbAtPathOnIOThread(
      ledger::DetachedPath db_path, CreateInStagingPath create_in_staging_path,
      fxl::RefPtr<DbInitializationState> initialization_state,
      fit::function<void(Status, std::unique_ptr<Db>)> callback);

  // Synchronously creates and initializes a new LevelDb instance in a two-step
  // process: the new instance is created in a temporary directory under the
  // staging path and, if successful, it is then moved to the given |db_path|.
  // This way, if initialization is interrupted, the potentially corrupted
  // database will be in the staging area.
  Status CreateDbThroughStagingPathOnIOThread(
      ledger::DetachedPath db_path, std::unique_ptr<LevelDb>* db);

  // Asynchronously creates and initializes a new LevelDb instance. Once done,
  // if there is a pending request, it responds to it.
  void PrepareCachedDb(CreateInStagingPath create_in_staging_path);

  // Uses the cached LevelDb instance to respond to the given request and
  // initializes a new LevelDb for the cache directory.
  void ReturnPrecachedDb(
      ledger::DetachedPath db_path,
      fit::function<void(Status, std::unique_ptr<Db>)> callback);

  // If the cached LevelDb instance is available, |cached_db_is_ready_| is set
  // to |true| and |cached_db_status_| and |cached_db_| are updated to hold the
  // returned values from the LevelDb creation.
  // If at any point there is failure in initializing cached db, i.e. when
  // |cached_db_status_| is not |OK|, LevelDbFactory stops trying to pre-cache
  // instances, and only tries to create them at the final destination.
  bool cached_db_is_ready_ = false;
  Status cached_db_status_ = Status::OK;
  std::unique_ptr<Db> cached_db_;

  // If a request is received before the cached db is ready, it is queued up, by
  // storing the requester's callback (|pending_request_|) and the path of the
  // final destination (|pending_request_path_|).
  fit::function<void(Status, std::unique_ptr<Db>)> pending_request_;
  ledger::DetachedPath pending_request_path_;

  ledger::Environment* environment_;
  // The path where new LevelDb instances are created, before they are moved to
  // their final destination, or the cached db path.
  const ledger::DetachedPath staging_path_;
  // The path that keeps the initialized cached instance of LevelDb.
  const ledger::DetachedPath cached_db_path_;
  coroutine::CoroutineManager coroutine_manager_;

  // This must be the last member of the class.
  fxl::WeakPtrFactory<LevelDbFactory> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LevelDbFactory);
};

}  // namespace storage

#endif  // PERIDOT_BIN_LEDGER_STORAGE_IMPL_LEVELDB_FACTORY_H_
