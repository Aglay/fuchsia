// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_LIB_COROUTINE_COROUTINE_MANAGER_H_
#define SRC_LEDGER_LIB_COROUTINE_COROUTINE_MANAGER_H_

#include <lib/fit/function.h>

#include <algorithm>
#include <list>
#include <queue>
#include <stack>

#include "src/ledger/lib/coroutine/coroutine.h"
#include "src/lib/callback/destruction_sentinel.h"

namespace coroutine {
// CoroutineManager manages the lifetime of coroutines.
//
// CoroutineManager is parametrized by the maximum number of tasks it runs concurrently:
// - if |max_coroutines| is 0, the number of tasks in unlimited.
// - otherwise, task are run on-demand until |max_coroutines| is reached, and then queued to execute
// once another task completes. In both cases, a new coroutine is created when a task starts
// executing, and discarded once its execution completes.
class CoroutineManager {
 public:
  explicit CoroutineManager(CoroutineService* service, size_t max_coroutines = 0)
      : max_coroutines_(max_coroutines), service_(service) {}

  ~CoroutineManager() { Shutdown(); }

  CoroutineManager(const CoroutineManager&) = delete;
  const CoroutineManager& operator=(const CoroutineManager&) = delete;

  // Starts a managed coroutine. This coroutine will be automatically
  // interrupted if this |CoroutineManager| object is destroyed.
  //
  // |callback| must be a callable object
  // |runnable| must be a callable object with the following signature:
  //   void(CoroutineHandler*, fit::function<void(Args...)>)
  // When the second argument of |runnable| is called, the coroutine is
  // unregistered from the manager object and |callback| is called with the same
  // arguments unless the manager is shutting down. It is an error to exit the
  // coroutine without calling |runnable|'s callback.
  template <typename Callback, typename Runnable>
  void StartCoroutine(Callback callback, Runnable runnable) {
    if (disabled_) {
      return;
    }
    StartOrEnqueueCoroutine([this, callback = std::move(callback),
                             runnable = std::move(runnable)](CoroutineHandler* handler) mutable {
      bool callback_called = false;
      auto iter = handlers_.insert(handlers_.cend(), handler);
      auto final_callback = [this, &callback_called, iter,
                             callback = std::move(callback)](auto... args) mutable {
        // Remove the handler before calling the final callback. Otherwise
        // the handler might be unnecessarily interrupted, if this object
        // destructor is called in the callback.
        handlers_.erase(iter);
        callback_called = true;
        if (!disabled_) {
          callback(std::move(args)...);
        }
      };

      runnable(handler, std::move(final_callback));

      // Verify that the handler is correctly unregistered. It would be a
      // bug otherwise.
      FXL_DCHECK(callback_called);
    });
  }

  // Starts a managed coroutine. This coroutine will be automatically
  // interrupted if this |CoroutineManager| object is destroyed.
  //
  // |runnable| must be a callable object with the following signature:
  //   void(CoroutineHandler*)
  template <typename Runnable>
  void StartCoroutine(Runnable runnable) {
    if (disabled_) {
      return;
    }
    StartOrEnqueueCoroutine(
        [this, runnable = std::move(runnable)](CoroutineHandler* handler) mutable {
          auto iter = handlers_.insert(handlers_.cend(), handler);
          runnable(handler);
          // `runnable` is not allowed to delete the coroutine manager that executes it, so
          // handlers_ is safe to access.
          handlers_.erase(iter);
        });
  }

  // Shuts the manager down. All running coroutines will be interrupted and any
  // future one will not be started.
  void Shutdown() {
    // Interrupt any active handlers.
    disabled_ = true;
    while (!handlers_.empty()) {
      (*handlers_.begin())->Resume(coroutine::ContinuationStatus::INTERRUPTED);
    }
    FXL_DCHECK(handlers_.empty());
  }

  // Enqueues |to_run|. Then either:
  // - immediately starts a new coroutine to run it if we have not reached |max_coroutines_|
  // concurrently running tasks.
  // - otherwise, enqueue it to be run once a task completes.
  void StartOrEnqueueCoroutine(fit::function<void(CoroutineHandler*)> to_run) {
    pending_tasks_.push(std::move(to_run));
    if (max_coroutines_ == 0 || handlers_.size() < max_coroutines_) {
      service_->StartCoroutine([this](CoroutineHandler* handler) { RunPending(handler); });
    }
  }

  // Runs a pending task with the current |handler| coroutine. Once it completes, start the next
  // pending task if this coroutine manager has not been destructed.
  void RunPending(CoroutineHandler* handler) {
    auto it = sentinels_.emplace(sentinels_.end());

    // Run the first available task.
    auto to_run = std::move(pending_tasks_.front());
    pending_tasks_.pop();

    if (it->DestructedWhile([to_run = std::move(to_run), handler] { to_run(handler); }) ||
        disabled_) {
      // Return early if this manager has been or is being destructed.
      return;
    }
    sentinels_.erase(it);
    // The coroutine might be interrupted now: we cannot yield on the handler.

    // Grab and run the next task if there is one.
    if (!pending_tasks_.empty()) {
      // Start a new coroutine. The current coroutine will terminate as soon as the new coroutine
      // yields.
      service_->StartCoroutine([this](CoroutineHandler* handler) { RunPending(handler); });
    }
  }

 private:
  // Maximum number of tasks to execute concurrently. If 0, unlimited.
  const size_t max_coroutines_;
  // Set to true when this manager is being destructed.
  bool disabled_ = false;
  // Currently allocated coroutines.
  std::list<coroutine::CoroutineHandler*> handlers_;
  // Each coroutine creates a sentinel to detect destruction of this coroutine manager.
  std::list<callback::DestructionSentinel> sentinels_;
  // Queue of pending tasks to execute when coroutines are available.
  std::queue<fit::function<void(CoroutineHandler*)>> pending_tasks_;
  CoroutineService* const service_;
};

}  // namespace coroutine

#endif  // SRC_LEDGER_LIB_COROUTINE_COROUTINE_MANAGER_H_
