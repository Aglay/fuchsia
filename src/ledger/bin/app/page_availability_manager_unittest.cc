// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/app/page_availability_manager.h"

#include <lib/callback/set_when_called.h>

#include "gtest/gtest.h"
#include "src/ledger/bin/testing/test_with_environment.h"

namespace ledger {
namespace {

using PageAvailabilityManagerTest = TestWithEnvironment;

TEST_F(PageAvailabilityManagerTest, PageAvailableByDefault) {
  bool on_empty_called;
  bool on_available_called;

  PageAvailabilityManager page_availability_manager;
  page_availability_manager.set_on_empty(callback::SetWhenCalled(&on_empty_called));
  page_availability_manager.OnPageAvailable(callback::SetWhenCalled(&on_available_called));

  EXPECT_TRUE(page_availability_manager.IsEmpty());
  EXPECT_TRUE(on_available_called);
  EXPECT_FALSE(on_empty_called);
}

TEST_F(PageAvailabilityManagerTest, BusyPage) {
  bool on_empty_called;
  bool on_available_called;

  PageAvailabilityManager page_availability_manager;
  page_availability_manager.set_on_empty(callback::SetWhenCalled(&on_empty_called));
  page_availability_manager.MarkPageBusy();
  page_availability_manager.OnPageAvailable(callback::SetWhenCalled(&on_available_called));

  EXPECT_FALSE(page_availability_manager.IsEmpty());
  EXPECT_FALSE(on_available_called);
  EXPECT_FALSE(on_empty_called);
}

TEST_F(PageAvailabilityManagerTest, PageAvailabilityManagerReusable) {
  bool on_empty_called;
  bool first_on_available_called;
  bool second_on_available_called;

  PageAvailabilityManager page_availability_manager;
  page_availability_manager.set_on_empty(callback::SetWhenCalled(&on_empty_called));
  page_availability_manager.MarkPageBusy();
  page_availability_manager.OnPageAvailable(callback::SetWhenCalled(&first_on_available_called));

  EXPECT_FALSE(page_availability_manager.IsEmpty());
  EXPECT_FALSE(first_on_available_called);
  EXPECT_FALSE(on_empty_called);

  page_availability_manager.OnPageAvailable(callback::SetWhenCalled(&second_on_available_called));
  EXPECT_FALSE(page_availability_manager.IsEmpty());
  EXPECT_FALSE(first_on_available_called);
  EXPECT_FALSE(second_on_available_called);
  EXPECT_FALSE(on_empty_called);

  page_availability_manager.MarkPageAvailable();

  EXPECT_TRUE(page_availability_manager.IsEmpty());
  EXPECT_TRUE(first_on_available_called);
  EXPECT_TRUE(second_on_available_called);
  EXPECT_TRUE(on_empty_called);

  page_availability_manager.set_on_empty(callback::SetWhenCalled(&on_empty_called));
  page_availability_manager.MarkPageBusy();
  page_availability_manager.OnPageAvailable(callback::SetWhenCalled(&second_on_available_called));
  page_availability_manager.OnPageAvailable(callback::SetWhenCalled(&first_on_available_called));

  EXPECT_FALSE(page_availability_manager.IsEmpty());
  EXPECT_FALSE(first_on_available_called);
  EXPECT_FALSE(second_on_available_called);
  EXPECT_FALSE(on_empty_called);

  page_availability_manager.MarkPageAvailable();

  EXPECT_TRUE(page_availability_manager.IsEmpty());
  EXPECT_TRUE(first_on_available_called);
  EXPECT_TRUE(second_on_available_called);
  EXPECT_TRUE(on_empty_called);
}

TEST_F(PageAvailabilityManagerTest, CallbacksNotCalledOnDestruction) {
  bool on_empty_called;
  bool first_on_available_called;
  bool second_on_available_called;

  auto page_availability_manager = std::make_unique<PageAvailabilityManager>();
  page_availability_manager->set_on_empty(callback::SetWhenCalled(&on_empty_called));
  page_availability_manager->MarkPageBusy();
  page_availability_manager->OnPageAvailable(callback::SetWhenCalled(&first_on_available_called));

  EXPECT_FALSE(page_availability_manager->IsEmpty());
  EXPECT_FALSE(first_on_available_called);
  EXPECT_FALSE(on_empty_called);

  page_availability_manager->OnPageAvailable(callback::SetWhenCalled(&second_on_available_called));

  EXPECT_FALSE(page_availability_manager->IsEmpty());
  EXPECT_FALSE(first_on_available_called);
  EXPECT_FALSE(second_on_available_called);
  EXPECT_FALSE(on_empty_called);

  page_availability_manager.reset();

  EXPECT_FALSE(first_on_available_called);
  EXPECT_FALSE(second_on_available_called);
  EXPECT_FALSE(on_empty_called);
}

}  // namespace
}  // namespace ledger
