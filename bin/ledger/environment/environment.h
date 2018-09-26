// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_ENVIRONMENT_ENVIRONMENT_H_
#define PERIDOT_BIN_LEDGER_ENVIRONMENT_ENVIRONMENT_H_

#include <memory>

#include <lib/async/dispatcher.h>
#include <lib/backoff/backoff.h>
#include <lib/fit/function.h>
#include <lib/timekeeper/clock.h>

#include "peridot/bin/ledger/coroutine/coroutine.h"
#include "peridot/lib/rng/random.h"

namespace ledger {

// Environment for the ledger application. |io_dispatcher| is optional, but if
// provided in the constructor, |dispatcher| must outlive |io_dispatcher|.
class Environment {
 public:
  using BackoffFactory = fit::function<std::unique_ptr<backoff::Backoff>()>;
  Environment(async_dispatcher_t* dispatcher, async_dispatcher_t* io_dispatcher,
              std::string firebase_api_key,
              std::unique_ptr<coroutine::CoroutineService> coroutine_service,
              BackoffFactory backoff_factory,
              std::unique_ptr<timekeeper::Clock> clock,
              std::unique_ptr<rng::Random> random);
  Environment(Environment&& other) noexcept;
  ~Environment();

  Environment& operator=(Environment&& other) noexcept;

  async_dispatcher_t* dispatcher() { return dispatcher_; }

  // Returns the async_dispatcher_t to be used for I/O operations.
  async_dispatcher_t* io_dispatcher() { return io_dispatcher_; }

  const std::string& firebase_api_key() { return firebase_api_key_; };

  coroutine::CoroutineService* coroutine_service() {
    return coroutine_service_.get();
  }

  std::unique_ptr<backoff::Backoff> MakeBackoff();

  timekeeper::Clock* clock() { return clock_.get(); }

  rng::Random* random() { return random_.get(); }

 private:
  async_dispatcher_t* dispatcher_ = nullptr;

  // The async_dispatcher_t to be used for I/O operations.
  async_dispatcher_t* io_dispatcher_ = nullptr;

  // The firebase API key.
  std::string firebase_api_key_;

  std::unique_ptr<coroutine::CoroutineService> coroutine_service_;
  BackoffFactory backoff_factory_;
  std::unique_ptr<timekeeper::Clock> clock_;
  std::unique_ptr<rng::Random> random_;
};

// Builder for the environment.
//
// The |SetAsync| method must be called before the environment can be build.
class EnvironmentBuilder {
 public:
  EnvironmentBuilder();
  ~EnvironmentBuilder();

  EnvironmentBuilder(const EnvironmentBuilder& other) = delete;
  EnvironmentBuilder(EnvironmentBuilder&& other) = delete;
  EnvironmentBuilder& operator=(const EnvironmentBuilder& other) = delete;
  EnvironmentBuilder& operator=(EnvironmentBuilder&& other) = delete;

  EnvironmentBuilder& SetAsync(async_dispatcher_t* dispatcher);
  EnvironmentBuilder& SetIOAsync(async_dispatcher_t* io_dispatcher);
  EnvironmentBuilder& SetFirebaseApiKey(std::string firebase_api_key);
  EnvironmentBuilder& SetCoroutineService(
      std::unique_ptr<coroutine::CoroutineService> coroutine_service);
  EnvironmentBuilder& SetBackoffFactory(
      Environment::BackoffFactory backoff_factory);
  EnvironmentBuilder& SetClock(std::unique_ptr<timekeeper::Clock> clock);
  EnvironmentBuilder& SetRandom(std::unique_ptr<rng::Random> random);

  Environment Build();

 private:
  async_dispatcher_t* dispatcher_ = nullptr;
  async_dispatcher_t* io_dispatcher_ = nullptr;
  std::string firebase_api_key_;
  std::unique_ptr<coroutine::CoroutineService> coroutine_service_;
  Environment::BackoffFactory backoff_factory_;
  std::unique_ptr<timekeeper::Clock> clock_;
  std::unique_ptr<rng::Random> random_;
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_ENVIRONMENT_ENVIRONMENT_H_
