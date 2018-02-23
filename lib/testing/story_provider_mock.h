// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_TESTING_STORY_PROVIDER_MOCK_H_
#define PERIDOT_LIB_TESTING_STORY_PROVIDER_MOCK_H_

#include <string>

#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fidl/cpp/bindings/interface_ptr_set.h"
#include "lib/story/fidl/story_provider.fidl.h"
#include "peridot/lib/testing/story_controller_mock.h"

namespace modular {

class StoryProviderMock : public StoryProvider {
 public:
  // Allows notification of watchers.
  void NotifyStoryChanged(modular::StoryInfoPtr story_info,
                          modular::StoryState story_state) {
    watchers_.ForAllPtrs(
        [&story_info, story_state](modular::StoryProviderWatcher* watcher) {
          watcher->OnChange(story_info->Clone(), story_state);
        });
  }

  const modular::StoryControllerMock& story_controller() const {
    return controller_mock_;
  }
  std::string last_created_story() const { return last_created_story_; }

 private:
  // |StoryProvider|
  void CreateStory(const f1dl::String& url,
                   const CreateStoryCallback& callback) override {
    last_created_story_ = url;
    callback("foo");
  }

  // |StoryProvider|
  void CreateStoryWithInfo(
      const f1dl::String& url,
      f1dl::Map<f1dl::String, f1dl::String> extra_info,
      const f1dl::String& json,
      const CreateStoryWithInfoCallback& callback) override {
    last_created_story_ = url;
    callback("foo");
  }

  // |StoryProvider|
  void Watch(
      f1dl::InterfaceHandle<modular::StoryProviderWatcher> watcher) override {
    watchers_.AddInterfacePtr(watcher.Bind());
  }

  // |StoryProvider|
  void DeleteStory(const f1dl::String& story_id,
                   const DeleteStoryCallback& callback) override {
    callback();
  }

  // |StoryProvider|
  void GetStoryInfo(const f1dl::String& story_id,
                    const GetStoryInfoCallback& callback) override {
    callback(nullptr);
  }

  // |StoryProvider|
  void GetController(
      const f1dl::String& story_id,
      f1dl::InterfaceRequest<modular::StoryController> story) override {
    binding_set_.AddBinding(&controller_mock_, std::move(story));
  }

  // |StoryProvider|
  void PreviousStories(const PreviousStoriesCallback& callback) override {
    callback(f1dl::Array<f1dl::String>::New(0));
  }

  // |StoryProvider|
  void GetImportance(const GetImportanceCallback& callback) override {
    callback(f1dl::Map<f1dl::String, float>());
  }

  // |StoryProvider|
  void WatchImportance(
      f1dl::InterfaceHandle<modular::StoryImportanceWatcher> watcher) override {
    importance_watchers_.AddInterfacePtr(watcher.Bind());
  }

  // |StoryProvider|
  void RunningStories(const RunningStoriesCallback& callback) override {
    callback(f1dl::Array<f1dl::String>::New(0));
  }

  // |StoryProvider|
  void Duplicate(f1dl::InterfaceRequest<StoryProvider> request) override {
    FXL_LOG(FATAL) << "StoryProviderMock::Duplicate() not implemented.";
  }

  // |StoryProvider|
  void GetLinkPeer(const f1dl::String& story_id,
                   f1dl::Array<f1dl::String> module_path,
                   const f1dl::String& link_path,
                   f1dl::InterfaceRequest<Link> request) override {
    FXL_LOG(FATAL) << "StoryProviderMock::GetLinkPeer() not implemented.";
  }

  std::string last_created_story_;
  modular::StoryControllerMock controller_mock_;
  f1dl::BindingSet<modular::StoryController> binding_set_;
  f1dl::InterfacePtrSet<modular::StoryProviderWatcher> watchers_;
  f1dl::InterfacePtrSet<modular::StoryImportanceWatcher> importance_watchers_;
};

}  // namespace modular

#endif  // PERIDOT_LIB_TESTING_STORY_PROVIDER_MOCK_H_
