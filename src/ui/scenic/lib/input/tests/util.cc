// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/input/tests/util.h"

#include <lib/fidl/cpp/clone.h>
#include <lib/fostr/fidl/fuchsia/ui/input/formatting.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>

#include <unordered_set>

#include <hid/hid.h>

#include "src/ui/scenic/lib/scheduling/constant_frame_predictor.h"
#include "src/ui/scenic/lib/scheduling/default_frame_scheduler.h"
#include "src/ui/scenic/lib/scheduling/id.h"

namespace lib_ui_input_tests {

// Used to compare whether two values are nearly equal.
// 1000 times machine limits to account for scaling from [0,1] to viewing volume [0,1000].
constexpr float kEpsilon = std::numeric_limits<float>::epsilon() * 1000;

using InputCommand = fuchsia::ui::input::Command;
using ScenicEvent = fuchsia::ui::scenic::Event;
using escher::impl::CommandBufferSequencer;
using fuchsia::ui::input::InputEvent;
using fuchsia::ui::input::KeyboardEvent;
using fuchsia::ui::input::KeyboardEventPhase;
using fuchsia::ui::input::PointerEvent;
using fuchsia::ui::input::PointerEventPhase;
using fuchsia::ui::input::PointerEventType;
using fuchsia::ui::input::SendKeyboardInputCmd;
using fuchsia::ui::input::SendPointerInputCmd;
using fuchsia::ui::scenic::SessionListener;
using scenic_impl::GlobalId;
using scenic_impl::ResourceId;
using scenic_impl::Scenic;
using scenic_impl::display::Display;
using scenic_impl::gfx::Engine;
using scenic_impl::gfx::GfxSystem;
using scenic_impl::gfx::test::ReleaseFenceSignallerForTest;
using scenic_impl::input::InputSystem;
using scenic_impl::test::ScenicTest;
using scheduling::ConstantFramePredictor;
using scheduling::DefaultFrameScheduler;

SessionWrapper::SessionWrapper(Scenic* scenic) {
  fuchsia::ui::scenic::SessionPtr session_ptr;
  fidl::InterfaceHandle<SessionListener> listener_handle;
  fidl::InterfaceRequest<SessionListener> listener_request = listener_handle.NewRequest();
  scenic->CreateSession(session_ptr.NewRequest(), std::move(listener_handle));
  session_ = std::make_unique<scenic::Session>(std::move(session_ptr), std::move(listener_request));
  session_->set_event_handler(fit::bind_member(this, &SessionWrapper::OnEvent));
}

SessionWrapper::SessionWrapper(SessionWrapper&& original) {
  session_ = std::move(original.session_);
  session_->set_event_handler(fit::bind_member(this, &SessionWrapper::OnEvent));
  view_ref_ = std::move(original.view_ref_);
  view_ = std::move(original.view_);
  events_ = std::move(original.events_);
}

SessionWrapper::~SessionWrapper() {
  if (session_) {
    session_->Flush();  // Ensure Scenic receives all release commands.
  }
}

void SessionWrapper::OnEvent(std::vector<ScenicEvent> events) {
  for (ScenicEvent& event : events) {
    if (event.is_input()) {
      events_.push_back(std::move(event.input()));
    }
    // Ignore other event types for these tests.
  }
}

ResourceGraph::ResourceGraph(scenic::Session* session)
    : scene(session),
      camera(scene),
      renderer(session),
      layer(session),
      layer_stack(session),
      compositor(session) {
  renderer.SetCamera(camera);
  layer.SetRenderer(renderer);
  layer_stack.AddLayer(layer);
  compositor.SetLayerStack(layer_stack);
}

void InputSystemTest::RequestToPresent(scenic::Session* session) {
  session->Present(/*presentation time*/ 0, [](auto) {});
  RunLoopFor(zx::msec(20));  // Run until the next frame should have been scheduled.
}

std::pair<SessionWrapper, ResourceGraph> InputSystemTest::CreateScene() {
  SessionWrapper root_session(scenic());
  ResourceGraph root_resources(root_session.session());
  root_resources.layer.SetSize(test_display_width_px(), test_display_height_px());
  return {std::move(root_session), std::move(root_resources)};
}

void InputSystemTest::SetUpTestView(scenic::View* view) {
  scenic::Session* const session = view->session();

  scenic::ShapeNode shape(session);
  shape.SetTranslation(2, 2, 0);  // Center the shape within the View.
  view->AddChild(shape);

  scenic::Rectangle rec(session, 5, 5);  // Simple; no real GPU work.
  shape.SetShape(rec);

  scenic::Material material(session);
  shape.SetMaterial(material);

  RequestToPresent(session);
}

SessionWrapper InputSystemTest::CreateClient(const std::string& name,
                                             fuchsia::ui::views::ViewToken view_token) {
  SessionWrapper session_wrapper(scenic());
  auto pair = scenic::ViewRefPair::New();
  fuchsia::ui::views::ViewRef clone;
  pair.view_ref.Clone(&clone);
  session_wrapper.SetViewRef(std::move(clone));
  session_wrapper.SetView(
      std::make_unique<scenic::View>(session_wrapper.session(), std::move(view_token),
                                     std::move(pair.control_ref), std::move(pair.view_ref), name));
  SetUpTestView(session_wrapper.view());

  return session_wrapper;
}

void InputSystemTest::InitializeScenic(Scenic* scenic) {
  auto signaller = std::make_unique<ReleaseFenceSignallerForTest>();
  display_ = std::make_unique<Display>(
      /*id*/ 0, test_display_width_px(), test_display_height_px());
  auto frame_scheduler = std::make_shared<DefaultFrameScheduler>(
      display_->vsync_timing(),
      std::make_unique<ConstantFramePredictor>(/* static_vsync_offset */ zx::msec(5)));

  engine_ = std::make_unique<Engine>(context_provider_.context(), frame_scheduler,
                                     std::move(signaller), escher::EscherWeakPtr());
  frame_scheduler->SetFrameRenderer(engine_->GetWeakPtr());
  auto gfx = scenic->RegisterSystem<GfxSystem>(engine_.get(),
                                               /* sysmem */ nullptr,
                                               /* display_manager */ nullptr);
  frame_scheduler->AddSessionUpdater(gfx->GetWeakPtr());
  scenic_->SetFrameScheduler(frame_scheduler);

  input_system_ = scenic->RegisterSystem<InputSystem>(engine_->scene_graph());
  scenic->SetInitialized();
}

void InputSystemTest::TearDown() {
  // A clean teardown sequence is a little involved but possible.
  // 0. All resources are released (i.e. test scope closure, ~ResourceGraph).
  // 1. Sessions |Flush| their last resource-release cmds (i.e. test scope closure,
  //    ~SessionWrapper).
  // 2. Scenic runs the last resource-release cmds.
  RunLoopUntilIdle();
  // 3. Destroy Scenic before destroying the command buffer sequencer (CBS).
  //    This ensures no CBS listeners are active by the time CBS is destroyed.
  ScenicTest::TearDown();
  engine_.reset();
  display_.reset();
}

PointerCommandGenerator::PointerCommandGenerator(ResourceId compositor_id, uint32_t device_id,
                                                 uint32_t pointer_id, PointerEventType type)
    : compositor_id_(compositor_id) {
  blank_.device_id = device_id;
  blank_.pointer_id = pointer_id;
  blank_.type = type;
}

InputCommand PointerCommandGenerator::Add(float x, float y) {
  PointerEvent event;
  fidl::Clone(blank_, &event);
  event.phase = PointerEventPhase::ADD;
  event.x = x;
  event.y = y;
  return MakeInputCommand(event);
}

InputCommand PointerCommandGenerator::Down(float x, float y) {
  PointerEvent event;
  fidl::Clone(blank_, &event);
  event.phase = PointerEventPhase::DOWN;
  event.x = x;
  event.y = y;
  return MakeInputCommand(event);
}

InputCommand PointerCommandGenerator::Move(float x, float y) {
  PointerEvent event;
  fidl::Clone(blank_, &event);
  event.phase = PointerEventPhase::MOVE;
  event.x = x;
  event.y = y;
  return MakeInputCommand(event);
}

InputCommand PointerCommandGenerator::Up(float x, float y) {
  PointerEvent event;
  fidl::Clone(blank_, &event);
  event.phase = PointerEventPhase::UP;
  event.x = x;
  event.y = y;
  return MakeInputCommand(event);
}

InputCommand PointerCommandGenerator::Remove(float x, float y) {
  PointerEvent event;
  fidl::Clone(blank_, &event);
  event.phase = PointerEventPhase::REMOVE;
  event.x = x;
  event.y = y;
  return MakeInputCommand(event);
}

InputCommand PointerCommandGenerator::MakeInputCommand(PointerEvent event) {
  SendPointerInputCmd pointer_cmd;
  pointer_cmd.compositor_id = compositor_id_;
  pointer_cmd.pointer_event = std::move(event);

  InputCommand input_cmd;
  input_cmd.set_send_pointer_input(std::move(pointer_cmd));

  return input_cmd;
}

KeyboardCommandGenerator::KeyboardCommandGenerator(ResourceId compositor_id, uint32_t device_id)
    : compositor_id_(compositor_id) {
  blank_.device_id = device_id;
}

InputCommand KeyboardCommandGenerator::Pressed(uint32_t hid_usage, uint32_t modifiers) {
  KeyboardEvent event;
  fidl::Clone(blank_, &event);
  event.phase = KeyboardEventPhase::PRESSED;
  event.hid_usage = hid_usage;
  event.modifiers = modifiers;
  return MakeInputCommand(event);
}

InputCommand KeyboardCommandGenerator::Released(uint32_t hid_usage, uint32_t modifiers) {
  KeyboardEvent event;
  fidl::Clone(blank_, &event);
  event.phase = KeyboardEventPhase::RELEASED;
  event.hid_usage = hid_usage;
  event.modifiers = modifiers;
  return MakeInputCommand(event);
}

InputCommand KeyboardCommandGenerator::Cancelled(uint32_t hid_usage, uint32_t modifiers) {
  KeyboardEvent event;
  fidl::Clone(blank_, &event);
  event.phase = KeyboardEventPhase::CANCELLED;
  event.hid_usage = hid_usage;
  event.modifiers = modifiers;
  return MakeInputCommand(event);
}

InputCommand KeyboardCommandGenerator::Repeat(uint32_t hid_usage, uint32_t modifiers) {
  KeyboardEvent event;
  fidl::Clone(blank_, &event);
  event.phase = KeyboardEventPhase::REPEAT;
  event.hid_usage = hid_usage;
  event.modifiers = modifiers;
  return MakeInputCommand(event);
}

InputCommand KeyboardCommandGenerator::MakeInputCommand(KeyboardEvent event) {
  // Typically code point is inferred this same way by DeviceState.
  event.code_point = hid_map_key(event.hid_usage,
                                 event.modifiers & (fuchsia::ui::input::kModifierShift |
                                                    fuchsia::ui::input::kModifierCapsLock),
                                 qwerty_map);

  SendKeyboardInputCmd keyboard_cmd;
  keyboard_cmd.compositor_id = compositor_id_;
  keyboard_cmd.keyboard_event = std::move(event);

  InputCommand input_cmd;
  input_cmd.set_send_keyboard_input(std::move(keyboard_cmd));

  return input_cmd;
}

bool PointerMatches(const PointerEvent& event, uint32_t pointer_id, PointerEventPhase phase,
                    float x, float y, fuchsia::ui::input::PointerEventType type) {
  using fuchsia::ui::input::operator<<;

  bool result = true;
  if (event.type != type) {
    FX_LOGS(ERROR) << "  Actual type: " << event.type;
    FX_LOGS(ERROR) << "Expected type: " << type;
    result = false;
  }
  if (event.pointer_id != pointer_id) {
    FX_LOGS(ERROR) << "  Actual id: " << event.pointer_id;
    FX_LOGS(ERROR) << "Expected id: " << pointer_id;
    result = false;
  }
  if (event.phase != phase) {
    FX_LOGS(ERROR) << "  Actual phase: " << event.phase;
    FX_LOGS(ERROR) << "Expected phase: " << phase;
    result = false;
  }
  if (fabs(event.x - x) > kEpsilon) {
    FX_LOGS(ERROR) << "  Actual x: " << event.x;
    FX_LOGS(ERROR) << "Expected x: " << x;
    result = false;
  }
  if (fabs(event.y - y) > kEpsilon) {
    FX_LOGS(ERROR) << "  Actual y: " << event.y;
    FX_LOGS(ERROR) << "Expected y: " << y;
    result = false;
  }
  return result;
}

}  // namespace lib_ui_input_tests
