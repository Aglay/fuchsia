// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "gtest/gtest.h"
#include "lib/async/cpp/future.h"
#include "peridot/bin/user_runner/story_runner/session_storage.h"
#include "peridot/lib/ledger_client/page_id.h"
#include "peridot/lib/testing/test_with_ledger.h"

namespace fuchsia {
namespace modular {
namespace {

class SessionStorageTest : public testing::TestWithLedger {
 protected:
  std::unique_ptr<SessionStorage> CreateStorage(std::string page_id) {
    return std::make_unique<SessionStorage>(ledger_client(),
                                            MakePageId(page_id));
  }

  // Convenience method to create a story for the test cases where
  // we're not testing CreateStory().
  fidl::StringPtr CreateStory(SessionStorage* storage) {
    auto future_story = storage->CreateStory(nullptr /* extra */);
    bool done{};
    fidl::StringPtr story_id;
    future_story->Then([&](fidl::StringPtr id, ::ledger::PageId) {
      done = true;
      story_id = std::move(id);
    });
    RunLoopUntil([&] { return done; });

    return story_id;
  }
};

TEST_F(SessionStorageTest, Create_VerifyData) {
  // Create a single story, and verify that the data we have stored about it is
  // correct.
  auto storage = CreateStorage("page");

  fidl::VectorPtr<StoryInfoExtraEntry> extra_entries;
  StoryInfoExtraEntry entry;
  entry.key = "key1";
  entry.value = "value1";
  extra_entries->push_back(std::move(entry));

  entry.key = "key2";
  entry.value = "value2";
  extra_entries->push_back(std::move(entry));

  auto future_story = storage->CreateStory(std::move(extra_entries));
  bool done{};
  fidl::StringPtr story_id;
  ::ledger::PageId page_id;
  future_story->Then([&](fidl::StringPtr id, ::ledger::PageId page) {
    done = true;
    story_id = std::move(id);
    page_id = std::move(page);
  });
  RunLoopUntil([&] { return done; });

  // Get the StoryData for this story.
  auto future_data = storage->GetStoryData(story_id);
  done = false;
  modular::internal ::StoryData cached_data;
  future_data->Then([&](modular::internal ::StoryDataPtr data) {
    ASSERT_TRUE(data);

    EXPECT_EQ(story_id, data->story_info.id);
    ASSERT_TRUE(data->story_page_id);
    EXPECT_EQ(page_id, *data->story_page_id);
    EXPECT_TRUE(data->story_info.extra);
    EXPECT_EQ(2u, data->story_info.extra->size());
    EXPECT_EQ("key1", data->story_info.extra->at(0).key);
    EXPECT_EQ("value1", data->story_info.extra->at(0).value);
    EXPECT_EQ("key2", data->story_info.extra->at(1).key);
    EXPECT_EQ("value2", data->story_info.extra->at(1).value);

    done = true;

    cached_data = std::move(*data);
  });
  RunLoopUntil([&] { return done; });

  // Verify that GetAllStoryData() also returns the same information.
  fidl::VectorPtr<modular::internal ::StoryData> all_data;
  auto future_all_data = storage->GetAllStoryData();
  future_all_data->Then(
      [&](fidl::VectorPtr<modular::internal ::StoryData> data) {
        all_data = std::move(data);
      });
  RunLoopUntil([&] { return !!all_data; });

  EXPECT_EQ(1u, all_data->size());
  EXPECT_EQ(cached_data, all_data->at(0));
}

TEST_F(SessionStorageTest, CreateGetAllDelete) {
  // Create a single story, call GetAllStoryData() to show that it was created,
  // and then delete it.
  //
  // Pipeline all the calls such to show that we data consistency based on call
  // order.
  auto storage = CreateStorage("page");
  auto future_story = storage->CreateStory(nullptr);

  // Immediately after creation is complete, delete it.
  FuturePtr<> delete_done;
  future_story->Then([&](fidl::StringPtr id, ::ledger::PageId page_id) {
    delete_done = storage->DeleteStory(id);
  });

  auto future_data = storage->GetAllStoryData();
  fidl::VectorPtr<modular::internal ::StoryData> all_data;
  future_data->Then([&](fidl::VectorPtr<modular::internal ::StoryData> data) {
    all_data = std::move(data);
  });

  RunLoopUntil([&] { return !!all_data; });

  // Given the ordering, we expect the story we created to show up.
  EXPECT_EQ(1u, all_data->size());

  // But if we get all data again, we should see no stories.
  future_data = storage->GetAllStoryData();
  all_data.reset();
  future_data->Then([&](fidl::VectorPtr<modular::internal ::StoryData> data) {
    all_data = std::move(data);
  });
  RunLoopUntil([&] { return !!all_data; });
  EXPECT_EQ(0u, all_data->size());
}

TEST_F(SessionStorageTest, CreateMultipleAndDeleteOne) {
  // Create two stories.
  //
  // * Their ids should be different.
  // * They should get different Ledger page ids.
  // * If we GetAllStoryData() we should see both of them.
  auto storage = CreateStorage("page");

  auto future_story1 = storage->CreateStory(nullptr);
  auto future_story2 = storage->CreateStory(nullptr);

  auto wait = Future<fidl::StringPtr, ::ledger::PageId>::Wait(
      {future_story1, future_story2});

  bool done{};
  wait->Then([&] { done = true; });
  RunLoopUntil([&] { return done; });

  // |future_story1| and |future_story2| are complete, so we can get their
  // contents synchronously.
  fidl::StringPtr story1_id;
  ::ledger::PageId story1_pageid;
  future_story1->Then([&](fidl::StringPtr id, ::ledger::PageId page_id) {
    story1_id = std::move(id);
    story1_pageid = std::move(page_id);
  });

  fidl::StringPtr story2_id;
  ::ledger::PageId story2_pageid;
  future_story2->Then([&](fidl::StringPtr id, ::ledger::PageId page_id) {
    story2_id = std::move(id);
    story2_pageid = std::move(page_id);
  });

  EXPECT_NE(story1_id, story2_id);
  EXPECT_NE(story1_pageid, story2_pageid);

  auto future_data = storage->GetAllStoryData();
  fidl::VectorPtr<modular::internal ::StoryData> all_data;
  future_data->Then([&](fidl::VectorPtr<modular::internal ::StoryData> data) {
    all_data = std::move(data);
  });
  RunLoopUntil([&] { return !!all_data; });

  EXPECT_EQ(2u, all_data->size());

  // Now delete one of them, and we should see that GetAllStoryData() only
  // returns one entry.
  bool delete_done{};
  storage->DeleteStory(story1_id)->Then([&] { delete_done = true; });

  future_data = storage->GetAllStoryData();
  all_data.reset();
  future_data->Then([&](fidl::VectorPtr<modular::internal ::StoryData> data) {
    all_data = std::move(data);
  });
  RunLoopUntil([&] { return !!all_data; });

  EXPECT_TRUE(delete_done);
  EXPECT_EQ(1u, all_data->size());

  // TODO(thatguy): Verify that the story's page was also deleted.
  // MI4-1002
}

TEST_F(SessionStorageTest, UpdateLastFocusedTimestamp) {
  auto storage = CreateStorage("page");
  auto story_id = CreateStory(storage.get());

  storage->UpdateLastFocusedTimestamp(story_id, 10);
  auto future_data = storage->GetStoryData(story_id);
  bool done{};
  future_data->Then([&](modular::internal ::StoryDataPtr data) {
    EXPECT_EQ(10, data->story_info.last_focus_time);
    done = true;
  });
  RunLoopUntil([&] { return done; });
}

TEST_F(SessionStorageTest, ObserveCreateUpdateDelete_Local) {
  auto storage = CreateStorage("page");

  bool updated{};
  fidl::StringPtr updated_story_id;
  modular::internal ::StoryData updated_story_data;
  storage->set_on_story_updated(
      [&](fidl::StringPtr story_id, modular::internal ::StoryData story_data) {
        updated_story_id = std::move(story_id);
        updated_story_data = std::move(story_data);
        updated = true;
      });

  bool deleted{};
  fidl::StringPtr deleted_story_id;
  storage->set_on_story_deleted([&](fidl::StringPtr story_id) {
    deleted_story_id = std::move(story_id);
    deleted = true;
  });

  auto created_story_id = CreateStory(storage.get());
  RunLoopUntil([&] { return updated; });
  EXPECT_EQ(created_story_id, updated_story_id);
  EXPECT_EQ(created_story_id, updated_story_data.story_info.id);

  // Update something and see a new notification.
  storage->UpdateLastFocusedTimestamp(created_story_id, 42);
  updated = false;
  RunLoopUntil([&] { return updated; });
  EXPECT_EQ(created_story_id, updated_story_id);
  EXPECT_EQ(42, updated_story_data.story_info.last_focus_time);

  // Delete the story and expect to see a notification.
  storage->DeleteStory(created_story_id);
  RunLoopUntil([&] { return deleted; });
  EXPECT_EQ(created_story_id, deleted_story_id);
}

TEST_F(SessionStorageTest, ObserveCreateUpdateDelete_Remote) {
  // Just like above, but we're going to trigger all of the operations that
  // would cause a chagne notification on a different Ledger page connection to
  // simulate them happening on another device.
  auto storage = CreateStorage("page");
  auto remote_storage = CreateStorage("page");

  bool updated{};
  fidl::StringPtr updated_story_id;
  modular::internal ::StoryData updated_story_data;
  storage->set_on_story_updated(
      [&](fidl::StringPtr story_id, modular::internal ::StoryData story_data) {
        updated_story_id = std::move(story_id);
        updated_story_data = std::move(story_data);
        updated = true;
      });

  bool deleted{};
  fidl::StringPtr deleted_story_id;
  storage->set_on_story_deleted([&](fidl::StringPtr story_id) {
    deleted_story_id = std::move(story_id);
    deleted = true;
  });

  auto created_story_id = CreateStory(remote_storage.get());
  RunLoopUntil([&] { return updated; });
  EXPECT_EQ(created_story_id, updated_story_id);
  EXPECT_EQ(created_story_id, updated_story_data.story_info.id);

  // Update something and see a new notification.
  remote_storage->UpdateLastFocusedTimestamp(created_story_id, 42);
  updated = false;
  RunLoopUntil([&] { return updated; });
  EXPECT_EQ(created_story_id, updated_story_id);
  EXPECT_EQ(42, updated_story_data.story_info.last_focus_time);

  // Delete the story and expect to see a notification.
  remote_storage->DeleteStory(created_story_id);
  RunLoopUntil([&] { return deleted; });
  EXPECT_EQ(created_story_id, deleted_story_id);
}

}  // namespace
}  // namespace modular
}  // namespace fuchsia
