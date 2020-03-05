// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/bin/a11y_manager/app.h"

#include <fuchsia/accessibility/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/vfs/cpp/pseudo_dir.h>

#include "gtest/gtest.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/ui/a11y/bin/a11y_manager/tests/mocks/mock_color_transform_handler.h"
#include "src/ui/a11y/bin/a11y_manager/tests/mocks/mock_pointer_event_registry.h"
#include "src/ui/a11y/bin/a11y_manager/tests/mocks/mock_semantic_listener.h"
#include "src/ui/a11y/bin/a11y_manager/tests/mocks/mock_setui_accessibility.h"
#include "src/ui/a11y/bin/a11y_manager/tests/util/util.h"
#include "src/ui/a11y/lib/magnifier/tests/mocks/mock_magnification_handler.h"
#include "src/ui/a11y/lib/testing/input.h"
#include "src/ui/a11y/lib/util/util.h"

namespace accessibility_test {
namespace {

using fuchsia::accessibility::semantics::Attributes;
using fuchsia::accessibility::semantics::Node;
using fuchsia::accessibility::semantics::NodePtr;
using fuchsia::accessibility::semantics::Role;
using fuchsia::ui::input::accessibility::EventHandling;
using fuchsia::ui::input::accessibility::PointerEventListener;
using fuchsia::ui::input::accessibility::PointerEventListenerPtr;

const std::string kSemanticTreeSingle = "ID: 0 Label:Label A\n";
constexpr int kMaxLogBufferSize = 1024;

class AppUnitTest : public gtest::TestLoopFixture {
 public:
  AppUnitTest() { context_ = context_provider_.context(); }
  void SetUp() override {
    TestLoopFixture::SetUp();

    zx::eventpair::create(0u, &eventpair_, &eventpair_peer_);
    view_ref_ = fuchsia::ui::views::ViewRef({
        .reference = std::move(eventpair_),
    });
  }

  // Sends pointer events and returns the |handled| argument of the (last) resulting
  // |OnStreamHandled| invocation.
  //
  // Yo dawg, I heard you like pointer event listener pointers, so I took a pointer to your pointer
  // event listener pointer so you can receive events while you receive events (while honoring the
  // C++ style guide).
  std::optional<EventHandling> SendPointerEvents(PointerEventListenerPtr* listener,
                                                 const std::vector<PointerParams>& events) {
    std::optional<EventHandling> event_handling;
    listener->events().OnStreamHandled =
        [&event_handling](uint32_t, uint32_t, EventHandling handled) { event_handling = handled; };

    for (const auto& params : events) {
      SendPointerEvent(listener->get(), params);
    }

    return event_handling;
  }

  void SendPointerEvent(PointerEventListener* listener, const PointerParams& params) {
    listener->OnEvent(ToPointerEvent(params, input_event_time_++));

    // Simulate trivial passage of time (can expose edge cases with posted async tasks).
    RunLoopUntilIdle();
  }

  // Sends a gesture that wouldn't be recognized by any accessibility feature, for testing arena
  // configuration.
  std::optional<EventHandling> SendUnrecognizedGesture(PointerEventListenerPtr* listener) {
    return SendPointerEvents(listener, Zip({TapEvents(1, {}), TapEvents(2, {})}));
  }

  zx::eventpair eventpair_, eventpair_peer_;
  sys::ComponentContext* context_;
  sys::testing::ComponentContextProvider context_provider_;
  fuchsia::ui::views::ViewRef view_ref_;

 private:
  // We don't actually use these times. If we did, we'd want to more closely correlate them with
  // fake time.
  uint64_t input_event_time_ = 0;
};

// Create a test node with only a node id and a label.
Node CreateTestNode(uint32_t node_id, std::string label) {
  Node node = Node();
  node.set_node_id(node_id);
  node.set_child_ids({});
  node.set_role(Role::UNKNOWN);
  node.set_attributes(Attributes());
  node.mutable_attributes()->set_label(std::move(label));
  fuchsia::ui::gfx::BoundingBox box;
  node.set_location(std::move(box));
  fuchsia::ui::gfx::mat4 transform;
  node.set_transform(std::move(transform));
  return node;
}

// Test to make sure ViewManager Service is exposed by A11y.
// Test sends a node update to ViewManager and then compare the expected
// result using log file created by semantics manager.
TEST_F(AppUnitTest, UpdateNodeToSemanticsManager) {
  a11y_manager::App app = a11y_manager::App(context_provider_.TakeContext());
  RunLoopUntilIdle();

  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection;
  fidl::Clone(view_ref_, &view_ref_connection);

  // Create ActionListener.
  accessibility_test::MockSemanticListener semantic_listener(&context_provider_,
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Creating test node to update.
  std::vector<Node> update_nodes;
  Node node = CreateTestNode(0, "Label A");
  Node clone_node;
  node.Clone(&clone_node);
  update_nodes.push_back(std::move(clone_node));

  // Update the node created above.
  semantic_listener.UpdateSemanticNodes(std::move(update_nodes));
  RunLoopUntilIdle();

  // Commit nodes.
  semantic_listener.CommitUpdates();
  RunLoopUntilIdle();

  // Check that the committed node is present in the semantic tree.
  vfs::PseudoDir* debug_dir = context_->outgoing()->debug_dir();
  vfs::internal::Node* test_node;
  ASSERT_EQ(ZX_OK, debug_dir->Lookup(std::to_string(a11y::GetKoid(view_ref_)), &test_node));

  char buffer[kMaxLogBufferSize];
  ReadFile(test_node, kSemanticTreeSingle.size(), buffer);
  EXPECT_EQ(kSemanticTreeSingle, buffer);
}

// This test makes sure that services implemented by the Tts manager are
// available.
TEST_F(AppUnitTest, OffersTtsManagerServices) {
  a11y_manager::App app = a11y_manager::App(context_provider_.TakeContext());
  RunLoopUntilIdle();
  fuchsia::accessibility::tts::TtsManagerPtr tts_manager;
  context_provider_.ConnectToPublicService(tts_manager.NewRequest());
  RunLoopUntilIdle();
  ASSERT_TRUE(tts_manager.is_bound());
}

TEST_F(AppUnitTest, NoListenerInitially) {
  MockPointerEventRegistry registry(&context_provider_);
  MockSetUIAccessibility setui(&context_provider_);
  a11y_manager::App app(context_provider_.TakeContext());

  setui.Set({}, [](auto) {});

  RunLoopUntilIdle();
  EXPECT_FALSE(registry.listener())
      << "No listener should be registered in the beginning, as there is no accessibility service "
         "enabled.";
}

TEST_F(AppUnitTest, ListenerForScreenReader) {
  MockPointerEventRegistry registry(&context_provider_);
  MockSetUIAccessibility setui(&context_provider_);
  a11y_manager::App app(context_provider_.TakeContext());
  EXPECT_FALSE(app.state().screen_reader_enabled());

  fuchsia::settings::AccessibilitySettings settings;
  settings.set_screen_reader(true);
  setui.Set(std::move(settings), [](auto) {});

  RunLoopUntilIdle();
  EXPECT_TRUE(app.state().screen_reader_enabled());

  ASSERT_TRUE(registry.listener());
  EXPECT_EQ(SendUnrecognizedGesture(&registry.listener()), EventHandling::CONSUMED);
}

TEST_F(AppUnitTest, ListenerForMagnifier) {
  MockPointerEventRegistry registry(&context_provider_);
  MockSetUIAccessibility setui(&context_provider_);
  a11y_manager::App app(context_provider_.TakeContext());

  fuchsia::settings::AccessibilitySettings settings;
  settings.set_enable_magnification(true);
  setui.Set(std::move(settings), [](auto) {});

  RunLoopUntilIdle();
  EXPECT_TRUE(app.state().magnifier_enabled());

  ASSERT_TRUE(registry.listener());
  EXPECT_EQ(SendUnrecognizedGesture(&registry.listener()), EventHandling::REJECTED);
}

TEST_F(AppUnitTest, ListenerForAll) {
  MockPointerEventRegistry registry(&context_provider_);
  MockSetUIAccessibility setui(&context_provider_);
  a11y_manager::App app(context_provider_.TakeContext());

  fuchsia::settings::AccessibilitySettings settings;
  settings.set_screen_reader(true);
  settings.set_enable_magnification(true);
  setui.Set(std::move(settings), [](auto) {});

  RunLoopUntilIdle();
  ASSERT_TRUE(registry.listener());
  EXPECT_EQ(SendUnrecognizedGesture(&registry.listener()), EventHandling::CONSUMED);
}

TEST_F(AppUnitTest, NoListenerAfterAllRemoved) {
  MockPointerEventRegistry registry(&context_provider_);
  MockSetUIAccessibility setui(&context_provider_);
  a11y_manager::App app(context_provider_.TakeContext());

  fuchsia::settings::AccessibilitySettings settings;
  settings.set_screen_reader(true);
  settings.set_enable_magnification(true);
  setui.Set(std::move(settings), [](auto) {});

  settings.set_screen_reader(false);
  settings.set_enable_magnification(false);
  setui.Set(std::move(settings), [](auto) {});

  RunLoopUntilIdle();
  EXPECT_FALSE(registry.listener());
}

// Covers a couple additional edge cases around removing listeners.
TEST_F(AppUnitTest, ListenerRemoveOneByOne) {
  MockPointerEventRegistry registry(&context_provider_);
  MockSetUIAccessibility setui(&context_provider_);
  a11y_manager::App app(context_provider_.TakeContext());

  fuchsia::settings::AccessibilitySettings settings;
  settings.set_screen_reader(true);
  settings.set_enable_magnification(true);
  setui.Set(std::move(settings), [](auto) {});

  settings.set_screen_reader(false);
  settings.set_enable_magnification(true);
  setui.Set(std::move(settings), [](auto) {});

  RunLoopUntilIdle();

  EXPECT_EQ(app.state().screen_reader_enabled(), false);
  EXPECT_EQ(app.state().magnifier_enabled(), true);

  ASSERT_TRUE(registry.listener());
  EXPECT_EQ(SendUnrecognizedGesture(&registry.listener()), EventHandling::REJECTED);

  settings.set_enable_magnification(false);
  setui.Set(std::move(settings), [](auto) {});

  RunLoopUntilIdle();
  EXPECT_FALSE(registry.listener());
}

// Makes sure gesture priorities are right. If they're not, screen reader would intercept this
// gesture.
TEST_F(AppUnitTest, MagnifierGestureWithScreenReader) {
  MockPointerEventRegistry registry(&context_provider_);
  MockSetUIAccessibility setui(&context_provider_);
  a11y_manager::App app(context_provider_.TakeContext());

  MockMagnificationHandler mag_handler;
  fidl::Binding<fuchsia::accessibility::MagnificationHandler> mag_handler_binding(&mag_handler);
  {
    fuchsia::accessibility::MagnifierPtr magnifier;
    context_provider_.ConnectToPublicService(magnifier.NewRequest());
    magnifier->RegisterHandler(mag_handler_binding.NewBinding());
  }

  fuchsia::settings::AccessibilitySettings settings;
  settings.set_screen_reader(true);
  settings.set_enable_magnification(true);
  setui.Set(std::move(settings), [](auto) {});

  SendPointerEvents(&registry.listener(), 3 * TapEvents(1, {}));
  RunLoopFor(a11y::Magnifier::kTransitionPeriod);

  EXPECT_GT(mag_handler.transform().scale, 1);
}

TEST_F(AppUnitTest, ColorCorrectionApplied) {
  // Create a mock color transform handler.
  MockColorTransformHandler mock_color_transform_handler(&context_provider_);

  // Create a mock setUI & configure initial settings (everything off).
  MockSetUIAccessibility mock_setui(&context_provider_);
  fuchsia::settings::AccessibilitySettings accessibilitySettings;
  accessibilitySettings.set_screen_reader(false);
  accessibilitySettings.set_color_inversion(false);
  accessibilitySettings.set_enable_magnification(false);
  accessibilitySettings.set_color_correction(fuchsia::settings::ColorBlindnessType::NONE);
  mock_setui.Set(std::move(accessibilitySettings), [](auto) {});
  a11y_manager::App app = a11y_manager::App(context_provider_.TakeContext());
  RunLoopUntilIdle();

  // Turn on color correction.
  fuchsia::settings::AccessibilitySettings newAccessibilitySettings;
  newAccessibilitySettings.set_color_correction(
      fuchsia::settings::ColorBlindnessType::DEUTERANOMALY);
  mock_setui.Set(std::move(newAccessibilitySettings), [](auto) {});
  RunLoopUntilIdle();

  // Verify that stuff changed
  EXPECT_EQ(fuchsia::accessibility::ColorCorrectionMode::CORRECT_DEUTERANOMALY,
            mock_color_transform_handler.GetColorCorrectionMode());
}

TEST_F(AppUnitTest, ColorInversionApplied) {
  // Create a mock color transform handler.
  MockColorTransformHandler mock_color_transform_handler(&context_provider_);

  // Create a mock setUI & configure initial settings (everything off).
  MockSetUIAccessibility mock_setui(&context_provider_);
  fuchsia::settings::AccessibilitySettings accessibilitySettings;
  accessibilitySettings.set_screen_reader(false);
  accessibilitySettings.set_color_inversion(false);
  accessibilitySettings.set_enable_magnification(false);
  accessibilitySettings.set_color_correction(fuchsia::settings::ColorBlindnessType::NONE);
  mock_setui.Set(std::move(accessibilitySettings), [](auto) {});
  a11y_manager::App app = a11y_manager::App(context_provider_.TakeContext());
  RunLoopUntilIdle();

  // Turn on color correction.
  fuchsia::settings::AccessibilitySettings newAccessibilitySettings;
  newAccessibilitySettings.set_color_inversion(true);
  mock_setui.Set(std::move(newAccessibilitySettings), [](auto) {});
  RunLoopUntilIdle();

  // Verify that stuff changed
  EXPECT_TRUE(mock_color_transform_handler.GetColorInversionEnabled());
}

TEST_F(AppUnitTest, ScreenReaderOnAtStartup) {
  // Create mock pointer event registry.
  MockPointerEventRegistry registry(&context_provider_);

  // Create a mock setUI & configure initial settings (screen reader on).
  MockSetUIAccessibility mock_setui(&context_provider_);
  fuchsia::settings::AccessibilitySettings accessibilitySettings;
  accessibilitySettings.set_screen_reader(true);
  accessibilitySettings.set_color_inversion(false);
  accessibilitySettings.set_enable_magnification(false);
  accessibilitySettings.set_color_correction(fuchsia::settings::ColorBlindnessType::NONE);
  mock_setui.Set(std::move(accessibilitySettings), [](auto) {});
  a11y_manager::App app = a11y_manager::App(context_provider_.TakeContext());
  RunLoopUntilIdle();

  // Verify that screen reader is on and the pointer event registry is wired up.
  EXPECT_TRUE(app.state().screen_reader_enabled());
  ASSERT_TRUE(registry.listener());
  EXPECT_EQ(SendUnrecognizedGesture(&registry.listener()), EventHandling::CONSUMED);
}

}  // namespace
}  // namespace accessibility_test
