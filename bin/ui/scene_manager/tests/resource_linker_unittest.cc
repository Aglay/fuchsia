// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/synchronization/waitable_event.h"
#include "lib/fsl/handles/object_info.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fsl/threading/thread.h"
#include "magenta/system/ulib/mx/include/mx/eventpair.h"

#include "lib/ui/scenic/fidl_helpers.h"
#include "lib/ui/tests/test_with_message_loop.h"
#include "garnet/bin/ui/scene_manager/resources/nodes/entity_node.h"
#include "garnet/bin/ui/scene_manager/resources/resource_linker.h"
#include "garnet/bin/ui/scene_manager/tests/session_test.h"
#include "garnet/bin/ui/scene_manager/tests/util.h"

namespace scene_manager {
namespace test {

using ResourceLinkerTest = SessionTest;

TEST_F(ResourceLinkerTest, HandleBehavior) {
  ResourceLinker linker;

  mx::eventpair destination;
  mx_handle_t source_handle;
  {
    mx::eventpair source;
    ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));
    source_handle = source.get();
  }
  // Source handle is dead.
  mx_koid_t import_koid = fsl::GetRelatedKoid(source_handle);
  ASSERT_EQ(MX_KOID_INVALID, import_koid);
}

TEST_F(ResourceLinkerTest, AllowsExport) {
  ResourceLinker linker;

  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  auto resource =
      fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

  ASSERT_TRUE(linker.ExportResource(resource.get(), std::move(source)));

  ASSERT_EQ(1u, linker.NumExports());
}

TEST_F(ResourceLinkerTest, AllowsImport) {
  ResourceLinker linker;

  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  auto exported =
      fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

  ASSERT_TRUE(linker.ExportResource(exported.get(), std::move(source)));

  ASSERT_EQ(1u, linker.NumExports());

  bool did_resolve = false;

  linker.SetOnImportResolvedCallback(
      [exported, &did_resolve](Import*, Resource* resource,
                               ImportResolutionResult cause) -> void {
        did_resolve = true;
        ASSERT_TRUE(resource);
        ASSERT_EQ(exported.get(), resource);
        ASSERT_NE(0u, resource->type_flags() & kEntityNode);
        ASSERT_EQ(ImportResolutionResult::kSuccess, cause);
      });
  ImportPtr import = fxl::MakeRefCounted<Import>(
      session_.get(), 2, scenic::ImportSpec::NODE, &linker);
  linker.ImportResource(import.get(),
                        scenic::ImportSpec::NODE,  // import spec
                        std::move(destination));   // import handle

  // Make sure the closure and its assertions are not skipped.
  ASSERT_TRUE(did_resolve);
  ASSERT_EQ(1u, linker.NumExports());
  ASSERT_EQ(0u, linker.NumUnresolvedImports());
}

TEST_F(ResourceLinkerTest, CannotImportWithDeadSourceAndDestinationHandles) {
  ResourceLinker linker;

  mx::eventpair destination_out;
  {
    mx::eventpair destination;
    mx::eventpair source;
    ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));
    destination_out = mx::eventpair{destination.get()};
    // source and destination dies now.
  }

  bool did_resolve = false;
  linker.SetOnImportResolvedCallback(
      [&did_resolve](Import* import, Resource* resource,
                     ImportResolutionResult cause) -> void {
        did_resolve = true;
      });
  ImportPtr import = fxl::MakeRefCounted<Import>(
      session_.get(), 1, scenic::ImportSpec::NODE, &linker);
  ASSERT_FALSE(
      linker.ImportResource(import.get(),
                            scenic::ImportSpec::NODE,      // import spec
                            std::move(destination_out)));  // import handle

  ASSERT_EQ(0u, linker.NumUnresolvedImports());
  ASSERT_FALSE(did_resolve);
}

TEST_F(ResourceLinkerTest, CannotImportWithDeadDestinationHandles) {
  ResourceLinker linker;

  mx::eventpair destination_out;
  mx::eventpair source;
  {
    mx::eventpair destination;

    ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));
    destination_out = mx::eventpair{destination.get()};
    // destination dies now.
  }

  bool did_resolve = false;
  linker.SetOnImportResolvedCallback(
      [&did_resolve](Import* import, Resource* resource,
                     ImportResolutionResult cause) -> void {
        did_resolve = true;
      });
  ImportPtr import = fxl::MakeRefCounted<Import>(
      session_.get(), 1, scenic::ImportSpec::NODE, &linker);
  ASSERT_FALSE(
      linker.ImportResource(import.get(),
                            scenic::ImportSpec::NODE,      // import spec
                            std::move(destination_out)));  // import handle

  ASSERT_EQ(0u, linker.NumUnresolvedImports());
  ASSERT_FALSE(did_resolve);
}

TEST_F(ResourceLinkerTest, CanImportWithDeadSourceHandle) {
  mx::eventpair destination;
  mx::eventpair source_out;
  {
    mx::eventpair source;

    ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));
    source_out = mx::eventpair{source.get()};
    // source dies now.
  }

  fsl::Thread thread;
  thread.Run();

  fxl::AutoResetWaitableEvent latch;
  ResourceLinker linker;
  scene_manager::ResourcePtr resource;
  ImportPtr import;

  thread.TaskRunner()->PostTask(fxl::MakeCopyable([
    this, &import, &resource, &linker, &latch,
    destination = std::move(destination)
  ]() mutable {

    // Set an expiry callback that checks the resource expired for the right
    // reason and signal the latch.
    linker.SetOnExpiredCallback(
        [&linker, &latch](Resource*, ResourceLinker::ExpirationCause cause) {
          ASSERT_EQ(ResourceLinker::ExpirationCause::kExportHandleClosed,
                    cause);
          ASSERT_EQ(0u, linker.NumUnresolvedImports());
          ASSERT_EQ(0u, linker.NumExports());
          latch.Signal();
        });

    bool did_resolve = false;
    linker.SetOnImportResolvedCallback(
        [&did_resolve](Import* import, Resource* resource,
                       ImportResolutionResult cause) -> void {
          did_resolve = true;
        });
    import = fxl::MakeRefCounted<Import>(session_.get(), 1,
                                         scenic::ImportSpec::NODE, &linker);
    ASSERT_TRUE(
        linker.ImportResource(import.get(),
                              scenic::ImportSpec::NODE,  // import spec
                              std::move(destination)));  // import handle

    ASSERT_EQ(1u, linker.NumUnresolvedImports());
    ASSERT_FALSE(did_resolve);
  }));

  latch.Wait();

  thread.TaskRunner()->PostTask(
      []() { fsl::MessageLoop::GetCurrent()->QuitNow(); });

  thread.Join();
}

TEST_F(ResourceLinkerTest, CannotExportWithDeadSourceAndDestinationHandles) {
  ResourceLinker linker;

  mx::eventpair source_out;
  {
    mx::eventpair destination;
    mx::eventpair source;
    ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));
    source_out = mx::eventpair{source.get()};
    // source and destination dies now.
  }

  auto resource =
      fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);
  ASSERT_FALSE(linker.ExportResource(resource.get(), std::move(source_out)));
  ASSERT_EQ(0u, linker.NumExports());
}

TEST_F(ResourceLinkerTest, CannotExportWithDeadSourceHandle) {
  ResourceLinker linker;

  mx::eventpair destination;
  mx::eventpair source_out;
  {
    mx::eventpair source;
    ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));
    source_out = mx::eventpair{source.get()};
    // source dies now.
  }

  auto resource =
      fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

  ASSERT_FALSE(linker.ExportResource(resource.get(), std::move(source_out)));
  ASSERT_EQ(0u, linker.NumExports());
}

// Related koid of the source handle is valid as long as the source handle
// itself is valid (i.e. it doesn't matter if the destination handle is dead).
TEST_F(ResourceLinkerTest, CanExportWithDeadDestinationHandle) {
  ResourceLinker linker;

  mx::eventpair source;
  {
    mx::eventpair destination;
    ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));
    // destination dies now.
  }

  fsl::Thread thread;
  thread.Run();

  fxl::AutoResetWaitableEvent latch;
  scene_manager::ResourcePtr resource;

  thread.TaskRunner()->PostTask(fxl::MakeCopyable([
    this, &resource, &linker, &latch, source = std::move(source)
  ]() mutable {
    resource =
        fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

    ASSERT_TRUE(linker.ExportResource(resource.get(), std::move(source)));
    ASSERT_EQ(1u, linker.NumExports());

    // Set an expiry callback that checks the resource expired for the right
    // reason and signal the latch.
    linker.SetOnExpiredCallback(
        [&linker, &latch](Resource*, ResourceLinker::ExpirationCause cause) {
          ASSERT_EQ(ResourceLinker::ExpirationCause::kNoImportsBound, cause);
          ASSERT_EQ(0u, linker.NumUnresolvedImports());
          ASSERT_EQ(0u, linker.NumExports());
          latch.Signal();
        });
  }));

  latch.Wait();

  thread.TaskRunner()->PostTask(
      []() { fsl::MessageLoop::GetCurrent()->QuitNow(); });

  thread.Join();
}

TEST_F(ResourceLinkerTest,
       DestinationHandleDeathAutomaticallyCleansUpResourceExport) {
  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  fsl::Thread thread;
  thread.Run();

  fxl::AutoResetWaitableEvent latch;
  ResourceLinker linker;
  scene_manager::ResourcePtr resource;

  thread.TaskRunner()->PostTask(fxl::MakeCopyable([
    this, &resource, &linker, &latch, source = std::move(source), &destination
  ]() mutable {
    // Register the resource.
    resource =
        fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

    ASSERT_TRUE(linker.ExportResource(resource.get(), std::move(source)));
    ASSERT_EQ(1u, linker.NumExports());

    // Set an expiry callback that checks the resource expired for the right
    // reason and signal the latch.
    linker.SetOnExpiredCallback(
        [&linker, &latch](Resource*, ResourceLinker::ExpirationCause cause) {
          ASSERT_EQ(ResourceLinker::ExpirationCause::kNoImportsBound, cause);
          ASSERT_EQ(0u, linker.NumExports());
          latch.Signal();
        });

    // Release the destination handle.
    destination.reset();
  }));

  latch.Wait();

  thread.TaskRunner()->PostTask(
      []() { fsl::MessageLoop::GetCurrent()->QuitNow(); });

  thread.Join();
}

TEST_F(ResourceLinkerTest,
       SourceHandleDeathAutomaticallyCleansUpUnresolvedImports) {
  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  fsl::Thread thread;
  thread.Run();

  fxl::AutoResetWaitableEvent latch;
  ResourceLinker linker;
  scene_manager::ResourcePtr resource;
  ImportPtr import;

  thread.TaskRunner()->PostTask(fxl::MakeCopyable([
    this, &import, &resource, &linker, &latch, source = std::move(source),
    &destination
  ]() mutable {
    // Register the resource.
    resource =
        fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

    // Import.
    bool did_resolve = false;
    linker.SetOnImportResolvedCallback(
        [&did_resolve, &linker, &latch](Import* import, Resource* resource,
                                        ImportResolutionResult cause) -> void {
          did_resolve = true;
          ASSERT_FALSE(resource);
          ASSERT_EQ(ImportResolutionResult::kExportHandleDiedBeforeBind, cause);
          ASSERT_EQ(0u, linker.NumUnresolvedImports());
          latch.Signal();
        });

    import = fxl::MakeRefCounted<Import>(session_.get(), 2,
                                         scenic::ImportSpec::NODE, &linker);
    linker.ImportResource(import.get(),
                          scenic::ImportSpec::NODE,     // import spec
                          CopyEventPair(destination));  // import handle

    ASSERT_EQ(1u, linker.NumUnresolvedImports());

    // Release both destination and source handles.
    destination.reset();
    source.reset();
  }));

  latch.Wait();

  thread.TaskRunner()->PostTask(
      []() { fsl::MessageLoop::GetCurrent()->QuitNow(); });

  thread.Join();
}

TEST_F(ResourceLinkerTest, ResourceDeathAutomaticallyCleansUpResourceExport) {
  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  fsl::Thread thread;
  thread.Run();

  fxl::AutoResetWaitableEvent latch;
  ResourceLinker linker;

  thread.TaskRunner()->PostTask(fxl::MakeCopyable([
    this, &linker, &latch, source = std::move(source), &destination
  ]() mutable {

    // Register the resource.
    auto resource =
        fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);
    ASSERT_TRUE(linker.ExportResource(resource.get(), std::move(source)));
    ASSERT_EQ(1u, linker.NumExports());

    // Set an expiry callback that checks the resource expired for the right
    // reason and signal the latch.
    linker.SetOnExpiredCallback(
        [&linker, &latch](Resource*, ResourceLinker::ExpirationCause cause) {
          ASSERT_EQ(ResourceLinker::ExpirationCause::kResourceDestroyed, cause);
          ASSERT_EQ(0u, linker.NumExports());
          latch.Signal();
        });

    // |resource| gets destroyed now since its out of scope.
  }));

  latch.Wait();

  thread.TaskRunner()->PostTask(
      []() { fsl::MessageLoop::GetCurrent()->QuitNow(); });

  thread.Join();
}

TEST_F(ResourceLinkerTest, ImportsBeforeExportsAreServiced) {
  ResourceLinker linker;

  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  auto exported =
      fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

  // Import.
  bool did_resolve = false;
  linker.SetOnImportResolvedCallback(
      [exported, &did_resolve](Import* import, Resource* resource,
                               ImportResolutionResult cause) -> void {
        did_resolve = true;
        ASSERT_TRUE(resource);
        ASSERT_EQ(exported.get(), resource);
        ASSERT_NE(0u, resource->type_flags() & kEntityNode);
        ASSERT_EQ(ImportResolutionResult::kSuccess, cause);
      });
  ImportPtr import = fxl::MakeRefCounted<Import>(
      session_.get(), 2, scenic::ImportSpec::NODE, &linker);
  linker.ImportResource(import.get(),
                        scenic::ImportSpec::NODE,  // import spec
                        std::move(destination));   // import handle

  ASSERT_FALSE(did_resolve);
  ASSERT_EQ(0u, linker.NumExports());
  ASSERT_EQ(1u, linker.NumUnresolvedImports());

  // Export.
  ASSERT_TRUE(linker.ExportResource(exported.get(), std::move(source)));
  ASSERT_EQ(1u, linker.NumExports());  // Since we already have the
                                       // destination handle in scope.
  ASSERT_EQ(0u, linker.NumUnresolvedImports());
  ASSERT_TRUE(did_resolve);
}

TEST_F(ResourceLinkerTest, ImportAfterReleasedExportedResourceFails) {
  ResourceLinker linker;

  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  bool did_resolve = false;
  {
    auto exported =
        fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

    // Import.
    linker.SetOnImportResolvedCallback(
        [&did_resolve](Import* import, Resource* resource,
                       ImportResolutionResult cause) -> void {
          did_resolve = true;
          ASSERT_EQ(nullptr, resource);
          ASSERT_EQ(ImportResolutionResult::kExportHandleDiedBeforeBind, cause);
        });

    // Export.
    ASSERT_TRUE(linker.ExportResource(exported.get(), std::move(source)));
    ASSERT_EQ(1u, linker.NumExports());  // Since we already have the
                                         // destination handle in scope.
    ASSERT_EQ(0u, linker.NumUnresolvedImports());

    // Release the exported resource.
  }
  ASSERT_EQ(0u, linker.NumExports());

  // Now try to import. We should get a resolution callback that it failed.
  ImportPtr import = fxl::MakeRefCounted<Import>(
      session_.get(), 2, scenic::ImportSpec::NODE, &linker);
  linker.ImportResource(import.get(),
                        scenic::ImportSpec::NODE,  // import spec
                        std::move(destination));   // import handle
  RUN_MESSAGE_LOOP_UNTIL(did_resolve);
  ASSERT_TRUE(did_resolve);
  ASSERT_EQ(0u, linker.NumUnresolvedImports());
}

TEST_F(ResourceLinkerTest, DuplicatedDestinationHandlesAllowMultipleImports) {
  ResourceLinker linker;

  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  auto exported =
      fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

  // Import multiple times.
  size_t resolution_count = 0;
  linker.SetOnImportResolvedCallback(
      [exported, &resolution_count](Import* import, Resource* resource,
                                    ImportResolutionResult cause) -> void {
        ASSERT_EQ(ImportResolutionResult::kSuccess, cause);
        resolution_count++;
        ASSERT_TRUE(resource);
        ASSERT_EQ(exported.get(), resource);
        ASSERT_NE(0u, resource->type_flags() & kEntityNode);
      });

  static const size_t kImportCount = 100;

  std::vector<ImportPtr> imports;
  for (size_t i = 1; i <= kImportCount; ++i) {
    mx::eventpair duplicate_destination = CopyEventPair(destination);

    ImportPtr import = fxl::MakeRefCounted<Import>(
        session_.get(), i + 1, scenic::ImportSpec::NODE, &linker);
    // Need to keep the import alive.
    imports.push_back(import);
    linker.ImportResource(import.get(),
                          scenic::ImportSpec::NODE,           // import spec
                          std::move(duplicate_destination));  // import handle

    ASSERT_EQ(0u, resolution_count);
    ASSERT_EQ(0u, linker.NumExports());
    ASSERT_EQ(i, linker.NumUnresolvedImports());
  }

  // Export.
  ASSERT_TRUE(linker.ExportResource(exported.get(), std::move(source)));
  ASSERT_EQ(1u, linker.NumExports());  // Since we already have the
                                       // destination handle in scope.
  ASSERT_EQ(0u, linker.NumUnresolvedImports());
  ASSERT_EQ(kImportCount, resolution_count);
}

TEST_F(ResourceLinkerTest, UnresolvedImportIsRemovedIfDestroyed) {
  ResourceLinker linker;

  mx::eventpair source, destination;
  ASSERT_EQ(MX_OK, mx::eventpair::create(0, &source, &destination));

  auto exported =
      fxl::MakeRefCounted<EntityNode>(session_.get(), 1 /* resource id */);

  // Import multiple times.
  size_t resolution_count = 0;
  linker.SetOnImportResolvedCallback(
      [exported, &resolution_count](Import* import, Resource* resource,
                                    ImportResolutionResult cause) -> void {
        ASSERT_EQ(ImportResolutionResult::kImportDestroyedBeforeBind, cause);
        resolution_count++;
      });

  static const size_t kImportCount = 2;

  for (size_t i = 1; i <= kImportCount; ++i) {
    mx::eventpair duplicate_destination = CopyEventPair(destination);

    ImportPtr import = fxl::MakeRefCounted<Import>(
        session_.get(), i + 1, scenic::ImportSpec::NODE, &linker);
    linker.ImportResource(import.get(),
                          scenic::ImportSpec::NODE,           // import spec
                          std::move(duplicate_destination));  // import handle

    ASSERT_EQ(0u, linker.NumExports());
    ASSERT_EQ(1u, linker.NumUnresolvedImports());
  }

  ASSERT_EQ(0u, linker.NumUnresolvedImports());
  ASSERT_EQ(kImportCount, resolution_count);
}

}  // namespace test
}  // namespace scene_manager
