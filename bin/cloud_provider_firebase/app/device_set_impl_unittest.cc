// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firebase/app/device_set_impl.h"

#include "garnet/lib/gtest/test_with_message_loop.h"
#include "lib/cloud_provider/fidl/cloud_provider.fidl.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/cloud_provider_firebase/device_set/testing/test_cloud_device_set.h"
#include "peridot/lib/callback/capture.h"
#include "peridot/lib/convert/convert.h"
#include "peridot/lib/firebase_auth/testing/test_firebase_auth.h"

namespace cloud_provider_firebase {

std::unique_ptr<TestCloudDeviceSet> InitCloudDeviceSet(
    TestCloudDeviceSet** ptr,
    fxl::RefPtr<fxl::TaskRunner> task_runner) {
  auto ret = std::make_unique<TestCloudDeviceSet>(task_runner);
  *ptr = ret.get();
  return ret;
}

class DeviceSetImplTest : public gtest::TestWithMessageLoop,
                          cloud_provider::DeviceSetWatcher {
 public:
  DeviceSetImplTest()
      : firebase_auth_(message_loop_.task_runner()),
        device_set_impl_(
            &firebase_auth_,
            InitCloudDeviceSet(&cloud_device_set_, message_loop_.task_runner()),
            device_set_.NewRequest()),
        watcher_binding_(this) {}

  ~DeviceSetImplTest() override {}

  // cloud_provider::DeviceSetWatcher:
  void OnCloudErased() override {
    on_cloud_erased_calls_++;
    message_loop_.PostQuitTask();
  }

  void OnNetworkError() override {
    on_network_error_calls_++;
    message_loop_.PostQuitTask();
  }

 protected:
  firebase_auth::TestFirebaseAuth firebase_auth_;
  TestCloudDeviceSet* cloud_device_set_;
  cloud_provider::DeviceSetPtr device_set_;
  DeviceSetImpl device_set_impl_;

  f1dl::Binding<cloud_provider::DeviceSetWatcher> watcher_binding_;
  int on_cloud_erased_calls_ = 0;
  int on_network_error_calls_ = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(DeviceSetImplTest);
};

TEST_F(DeviceSetImplTest, EmptyWhenDisconnected) {
  bool on_empty_called = false;
  device_set_impl_.set_on_empty([this, &on_empty_called] {
    on_empty_called = true;
    message_loop_.PostQuitTask();
  });
  device_set_.Unbind();
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(on_empty_called);
}

TEST_F(DeviceSetImplTest, CheckFingerprint) {
  cloud_device_set_->status_to_return = CloudDeviceSet::Status::OK;
  cloud_provider::Status status;
  device_set_->CheckFingerprint(convert::ToArray("bazinga"),
                                callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(cloud_provider::Status::OK, status);
  EXPECT_EQ("bazinga", cloud_device_set_->checked_fingerprint);
}

TEST_F(DeviceSetImplTest, SetFingerprint) {
  cloud_device_set_->status_to_return = CloudDeviceSet::Status::OK;
  cloud_provider::Status status;
  device_set_->SetFingerprint(convert::ToArray("bazinga"),
                              callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(cloud_provider::Status::OK, status);
  EXPECT_EQ("bazinga", cloud_device_set_->set_fingerprint);
}

TEST_F(DeviceSetImplTest, SetWatcher) {
  cloud_device_set_->status_to_return = CloudDeviceSet::Status::OK;
  cloud_provider::Status status;
  cloud_provider::DeviceSetWatcherPtr watcher;
  watcher_binding_.Bind(watcher.NewRequest());
  device_set_->SetWatcher(convert::ToArray("bazinga"), std::move(watcher),
                          callback::Capture(MakeQuitTask(), &status));
  EXPECT_TRUE(RunLoopUntil(
      [this] { return cloud_device_set_->watch_callback != nullptr; }));
  EXPECT_EQ("bazinga", cloud_device_set_->watched_fingerprint);
  EXPECT_EQ(0, cloud_device_set_->timestamp_update_requests_);

  // Call the callback the first time confirming that it was correctly set.
  cloud_device_set_->watch_callback(CloudDeviceSet::Status::OK);
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(cloud_provider::Status::OK, status);
  EXPECT_EQ(0, on_cloud_erased_calls_);
  EXPECT_EQ(0, on_network_error_calls_);
  EXPECT_EQ(1, cloud_device_set_->timestamp_update_requests_);

  // Call the callback the second time signalling that the cloud was erased.
  cloud_device_set_->watch_callback(CloudDeviceSet::Status::ERASED);
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(1, on_cloud_erased_calls_);
  EXPECT_EQ(0, on_network_error_calls_);
  EXPECT_EQ(1, cloud_device_set_->timestamp_update_requests_);
}

TEST_F(DeviceSetImplTest, SetWatcherFailToSet) {
  cloud_device_set_->status_to_return = CloudDeviceSet::Status::OK;
  cloud_provider::Status status;
  cloud_provider::DeviceSetWatcherPtr watcher;
  watcher_binding_.Bind(watcher.NewRequest());
  device_set_->SetWatcher(convert::ToArray("bazinga"), std::move(watcher),
                          callback::Capture(MakeQuitTask(), &status));
  EXPECT_TRUE(RunLoopUntil(
      [this] { return cloud_device_set_->watch_callback != nullptr; }));

  // Call the callback indicating the network error. This should result both in
  // the returned error status being NETWORK_ERROR and the OnNetworkError()
  // watcher method being called.
  cloud_device_set_->watch_callback(CloudDeviceSet::Status::NETWORK_ERROR);
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(cloud_provider::Status::NETWORK_ERROR, status);
  EXPECT_EQ(0, on_cloud_erased_calls_);
  EXPECT_EQ(1, on_network_error_calls_);
  EXPECT_EQ(0, cloud_device_set_->timestamp_update_requests_);
}

TEST_F(DeviceSetImplTest, Erase) {
  cloud_device_set_->status_to_return = CloudDeviceSet::Status::OK;
  cloud_provider::Status status;
  device_set_->CheckFingerprint(convert::ToArray("bazinga"),
                                callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(cloud_provider::Status::OK, status);
}

TEST_F(DeviceSetImplTest, EraseNetworkError) {
  cloud_device_set_->status_to_return = CloudDeviceSet::Status::NETWORK_ERROR;
  cloud_provider::Status status;
  device_set_->CheckFingerprint(convert::ToArray("bazinga"),
                                callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(cloud_provider::Status::NETWORK_ERROR, status);
}

}  // namespace cloud_provider_firebase
