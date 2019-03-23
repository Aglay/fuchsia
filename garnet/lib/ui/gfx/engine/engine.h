// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_ENGINE_ENGINE_H_
#define GARNET_LIB_UI_GFX_ENGINE_ENGINE_H_

#include <fbl/ref_ptr.h>
#include <lib/fit/function.h>
#include <set>
#include <vector>

#include "lib/component/cpp/startup_context.h"
#include "lib/escher/escher.h"
#include "lib/escher/flib/release_fence_signaller.h"
#include "lib/escher/resources/resource_recycler.h"
#include "lib/escher/shape/rounded_rect_factory.h"
#include "lib/escher/vk/image_factory.h"

#include "garnet/lib/ui/gfx/displays/display_manager.h"
#include "garnet/lib/ui/gfx/engine/engine_renderer.h"
#include "garnet/lib/ui/gfx/engine/frame_scheduler.h"
#include "garnet/lib/ui/gfx/engine/object_linker.h"
#include "garnet/lib/ui/gfx/engine/resource_linker.h"
#include "garnet/lib/ui/gfx/engine/scene_graph.h"
#include "garnet/lib/ui/gfx/engine/session_context.h"
#include "garnet/lib/ui/gfx/engine/session_manager.h"
#include "garnet/lib/ui/gfx/id.h"
#include "garnet/lib/ui/gfx/resources/import.h"
#include "garnet/lib/ui/gfx/resources/nodes/scene.h"
#include "garnet/lib/ui/gfx/util/event_timestamper.h"
#include "garnet/lib/ui/scenic/event_reporter.h"
#include "lib/escher/renderer/batch_gpu_uploader.h"

namespace scenic_impl {
namespace gfx {

class Compositor;
class FrameTimings;
using FrameTimingsPtr = fxl::RefPtr<FrameTimings>;
class Session;
class SessionHandler;
class View;
class ViewHolder;

using ViewLinker = ObjectLinker<ViewHolder, View>;

// Graphical context for a set of session updates.
// The CommandContext is only valid during RenderFrame() and should not be
// accessed outside of that.
class CommandContext {
 public:
  CommandContext(std::unique_ptr<escher::BatchGpuUploader> uploader);

  escher::BatchGpuUploader* batch_gpu_uploader() const {
    return batch_gpu_uploader_.get();
  }

  // Flush any work accumulated during command processing.
  void Flush();

 private:
  std::unique_ptr<escher::BatchGpuUploader> batch_gpu_uploader_;
};

// Owns a group of sessions which can share resources with one another
// using the same resource linker and which coexist within the same timing
// domain using the same frame scheduler.  It is not possible for sessions
// which belong to different engines to communicate with one another.
class Engine : public SessionUpdater, public FrameRenderer {
 public:
  Engine(component::StartupContext* startup_context,
         std::unique_ptr<FrameScheduler> frame_scheduler,
         DisplayManager* display_manager, escher::EscherWeakPtr escher);

  ~Engine() override = default;

  escher::Escher* escher() const { return escher_.get(); }
  escher::EscherWeakPtr GetEscherWeakPtr() const { return escher_; }

  vk::Device vk_device() {
    return escher_ ? escher_->vulkan_context().device : vk::Device();
  }

  bool has_vulkan() const { return has_vulkan_; }

  ResourceLinker* resource_linker() { return &resource_linker_; }
  ViewLinker* view_linker() { return &view_linker_; }

  SessionManager* session_manager() { return session_manager_.get(); }

  EngineRenderer* renderer() { return engine_renderer_.get(); }

  // TODO(SCN-1151)
  // Instead of a set of Compositors, we should probably root at a set of
  // Displays. Or, we might not even need to store this set, and Displays (or
  // Compositors) would just be able to schedule a frame for themselves.
  SceneGraphWeakPtr scene_graph() { return scene_graph_.GetWeakPtr(); }

  SessionContext session_context() {
    return SessionContext{vk_device(),
                          escher(),
                          imported_memory_type_index(),
                          escher_resource_recycler(),
                          escher_image_factory(),
                          escher_rounded_rect_factory(),
                          release_fence_signaller(),
                          event_timestamper(),
                          session_manager(),
                          frame_scheduler(),
                          display_manager_,
                          scene_graph(),
                          resource_linker(),
                          view_linker()};
  }

  // Invoke Escher::Cleanup().  If more work remains afterward, post a delayed
  // task to try again; this is typically because cleanup couldn't finish due
  // to unfinished GPU work.
  void CleanupEscher();

  // Dumps the contents of all scene graphs.
  std::string DumpScenes() const;

  // |SessionUpdater|
  //
  // Applies scheduled updates to a session. If the update fails, the session is
  // killed. Returns true if a new render is needed, false otherwise.
  bool UpdateSessions(std::vector<SessionUpdate> sessions_to_update,
                      uint64_t frame_number, uint64_t presentation_time,
                      uint64_t presentation_interval) override;

  // |FrameRenderer|
  //
  // Renders a new frame. Returns true if successful, false otherwise.
  bool RenderFrame(const FrameTimingsPtr& frame, uint64_t presentation_time,
                   uint64_t presentation_interval) override;

 protected:
  // Only used by subclasses used in testing.
  Engine(component::StartupContext* startup_context,
         std::unique_ptr<FrameScheduler> frame_scheduler,
         DisplayManager* display_manager,
         std::unique_ptr<escher::ReleaseFenceSignaller> release_fence_signaller,
         std::unique_ptr<SessionManager> session_manager,
         escher::EscherWeakPtr escher);

 private:
  void InitializeFrameScheduler();

  // Creates a command context.
  CommandContext CreateCommandContext(uint64_t frame_number_for_tracing);

  // Used by GpuMemory to import VMOs from clients.
  uint32_t imported_memory_type_index() const {
    return imported_memory_type_index_;
  }
  EventTimestamper* event_timestamper() { return &event_timestamper_; }
  FrameScheduler* frame_scheduler() { return frame_scheduler_.get(); }

  escher::ResourceRecycler* escher_resource_recycler() {
    return escher_ ? escher_->resource_recycler() : nullptr;
  }

  escher::ImageFactory* escher_image_factory() { return image_factory_.get(); }

  escher::RoundedRectFactory* escher_rounded_rect_factory() {
    return rounded_rect_factory_.get();
  }

  escher::ReleaseFenceSignaller* release_fence_signaller() {
    return release_fence_signaller_.get();
  }

  void InitializeShaderFs();

  // Update and deliver metrics for all nodes which subscribe to metrics
  // events.
  void UpdateAndDeliverMetrics(uint64_t presentation_time);

  // Update reported metrics for nodes which subscribe to metrics events.
  // If anything changed, append the node to |updated_nodes|.
  void UpdateMetrics(Node* node,
                     const ::fuchsia::ui::gfx::Metrics& parent_metrics,
                     std::vector<Node*>* updated_nodes);

  DisplayManager* const display_manager_;
  const escher::EscherWeakPtr escher_;

  std::unique_ptr<EngineRenderer> engine_renderer_;

  ResourceLinker resource_linker_;
  ViewLinker view_linker_;

  EventTimestamper event_timestamper_;
  std::unique_ptr<escher::ImageFactoryAdapter> image_factory_;
  std::unique_ptr<escher::RoundedRectFactory> rounded_rect_factory_;
  std::unique_ptr<escher::ReleaseFenceSignaller> release_fence_signaller_;
  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<FrameScheduler> frame_scheduler_;
  SceneGraph scene_graph_;

  bool escher_cleanup_scheduled_ = false;

  uint32_t imported_memory_type_index_ = 0;

  // Tracks the number of sessions returning ApplyUpdateResult::needs_render and
  // uses it for tracing.
  uint64_t needs_render_count_ = 0;
  uint64_t processed_needs_render_count_ = 0;

  bool render_continuously_ = false;
  bool has_vulkan_ = false;

  fxl::WeakPtrFactory<Engine> weak_factory_;  // must be last

  FXL_DISALLOW_COPY_AND_ASSIGN(Engine);
};

}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_ENGINE_ENGINE_H_
