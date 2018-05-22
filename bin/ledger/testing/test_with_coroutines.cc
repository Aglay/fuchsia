// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/testing/test_with_coroutines.h"

namespace test {

namespace {

// Wrapper around a real CoroutineHandler for test.
//
// The wrapper allows to delay re-entering the coroutine body when the run loop
// is running. When |Continue| is called, it quits the loop, and the main method
// calls |ContinueIfNeeded| when the loop exits.
class TestCoroutineHandler : public coroutine::CoroutineHandler {
 public:
  explicit TestCoroutineHandler(coroutine::CoroutineHandler* delegate,
                                fxl::Closure quit_callback)
      : delegate_(delegate), quit_callback_(std::move(quit_callback)) {}

  coroutine::ContinuationStatus Yield() override { return delegate_->Yield(); }

  void Continue(coroutine::ContinuationStatus status) override {
    // If interrupting, no need to delay the call as the test will not run the
    // loop itself.
    if (status == coroutine::ContinuationStatus::INTERRUPTED) {
      delegate_->Continue(status);
      return;
    }
    quit_callback_();
    need_to_continue_ = true;
  }

  // Re-enters the coroutine body if the handler delayed the call.
  void ContinueIfNeeded() {
    if (need_to_continue_) {
      need_to_continue_ = false;
      delegate_->Continue(coroutine::ContinuationStatus::OK);
    }
  }

 private:
  coroutine::CoroutineHandler* delegate_;
  fxl::Closure quit_callback_;
  bool need_to_continue_ = false;
};

}  // namespace

TestWithCoroutines::TestWithCoroutines() {}

void TestWithCoroutines::RunInCoroutine(
    std::function<void(coroutine::CoroutineHandler*)> run_test) {
  std::unique_ptr<TestCoroutineHandler> test_handler;
  volatile bool ended = false;
  fxl::Closure quit_callback = [this] { message_loop_.QuitNow(); };
  coroutine_service_.StartCoroutine(
      [&, quit_callback =
              std::move(quit_callback)](coroutine::CoroutineHandler* handler) {
        test_handler =
            std::make_unique<TestCoroutineHandler>(handler, quit_callback);
        run_test(test_handler.get());
        ended = true;
      });
  while (!ended) {
    test_handler->ContinueIfNeeded();
    RunLoopUntilIdle();
  }
}

}  // namespace test
