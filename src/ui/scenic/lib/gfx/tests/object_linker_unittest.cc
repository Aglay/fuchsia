// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/engine/object_linker.h"

#include <lib/fit/function.h>
#include <lib/zx/eventpair.h>
#include <zircon/types.h>

#include "gtest/gtest.h"
#include "src/ui/scenic/lib/gfx/tests/error_reporting_test.h"

namespace scenic_impl {
namespace gfx {
namespace test {

constexpr int kExportValue = 57;
constexpr int kImportValue = 42;

#define ERROR_IF_CALLED(str) \
  std::bind([]() { EXPECT_TRUE(false) << "Delegate called unexpectedly: " << str; })

class ObjectLinkerTest : public ErrorReportingTest {
 protected:
  struct TestExportObj {
    int value = 0;
    explicit TestExportObj(int value) : value(value) {}
  };
  struct TestImportObj {
    int value = 0;
    explicit TestImportObj(int value) : value(value) {}
  };
  using TestObjectLinker = ObjectLinker<TestExportObj, TestImportObj>;

  TestObjectLinker object_linker_;
};

TEST_F(ObjectLinkerTest, InitialState) {
  EXPECT_EQ(0u, object_linker_.ExportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedExportCount());
  EXPECT_EQ(0u, object_linker_.ImportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedImportCount());

  TestObjectLinker::ExportLink export_link;
  TestObjectLinker::ImportLink import_link;
  EXPECT_FALSE(export_link.valid());
  EXPECT_FALSE(import_link.valid());
  EXPECT_FALSE(export_link.initialized());
  EXPECT_FALSE(import_link.initialized());
}

TEST_F(ObjectLinkerTest, AllowsExport) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));

  TestExportObj export_obj(kExportValue);
  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(export_link.valid());
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());
}

TEST_F(ObjectLinkerTest, CannotExportInvalidToken) {
  TestExportObj export_obj(kExportValue);
  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), zx::eventpair(), error_reporter());
  EXPECT_ERROR_COUNT(1);  // CreateExport throws an error.
  EXPECT_FALSE(export_link.valid());
  EXPECT_EQ(0u, object_linker_.ExportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedExportCount());
}

TEST_F(ObjectLinkerTest, CannotExportWithDeadExportToken) {
  zx::eventpair export_token, import_token;
  {
    zx::eventpair export_token2;
    EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token2, &import_token));
    export_token = zx::eventpair{export_token2.get()};
    // |export_token2| dies now, |export_token| is an invalid copy.
  }

  TestExportObj export_obj(kExportValue);
  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  EXPECT_ERROR_COUNT(1);  // CreateExport throws an error.
  EXPECT_FALSE(export_link.valid());
  EXPECT_EQ(0u, object_linker_.ExportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedExportCount());
}

TEST_F(ObjectLinkerTest, CanExportWithDeadImportToken) {
  zx::eventpair export_token, import_token;
  {
    zx::eventpair import_token2;
    EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token2));
    import_token = zx::eventpair{import_token2.get()};
    // |import_token2| dies now, |import_token| is an invalid copy.
  }

  TestExportObj export_obj(kExportValue);
  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(export_link.valid());
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());
}

TEST_F(ObjectLinkerTest, CannotExportSameTokenTwice) {
  zx::eventpair export_token, export_token2, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
  EXPECT_EQ(ZX_OK, export_token.duplicate(ZX_RIGHT_SAME_RIGHTS, &export_token2));

  TestExportObj export_obj(kExportValue);
  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(export_link.valid());
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());

  TestExportObj export_obj2(kExportValue);
  TestObjectLinker::ExportLink export_link2 = object_linker_.CreateExport(
      std::move(export_obj2), std::move(export_token2), error_reporter());
  EXPECT_ERROR_COUNT(1);  // CreateExport throws an error.
  EXPECT_FALSE(export_link2.valid());
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());
}

TEST_F(ObjectLinkerTest, LinkDeathRemovesExport) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));

  {
    TestExportObj export_obj(kExportValue);
    TestObjectLinker::ExportLink export_link = object_linker_.CreateExport(
        std::move(export_obj), std::move(export_token), error_reporter());
    EXPECT_ERROR_COUNT(0);
    EXPECT_TRUE(export_link.valid());
    EXPECT_EQ(1u, object_linker_.ExportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());

    // | export_link dies now. |
  }

  EXPECT_EQ(0u, object_linker_.ExportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedExportCount());
}

TEST_F(ObjectLinkerTest, AllowsImport) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));

  TestImportObj import_obj(kImportValue);
  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(import_link.valid());
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());
}

TEST_F(ObjectLinkerTest, CannotImportInvalidToken) {
  zx::eventpair import_token{ZX_HANDLE_INVALID};

  TestImportObj import_obj(kImportValue);
  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  EXPECT_ERROR_COUNT(1);  // CreateImport throws an error.
  EXPECT_FALSE(import_link.valid());
  EXPECT_EQ(0u, object_linker_.ImportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedImportCount());
}

TEST_F(ObjectLinkerTest, CannotImportWithDeadImportToken) {
  zx::eventpair export_token, import_token;
  {
    zx::eventpair import_token2;
    EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token2));
    import_token = zx::eventpair{import_token2.get()};
    // |import_token2| dies now, |import_token| is an invalid copy.
  }

  TestImportObj import_obj(kImportValue);
  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  EXPECT_ERROR_COUNT(1);  // CreateImport throws an error.
  EXPECT_FALSE(import_link.valid());
  EXPECT_EQ(0u, object_linker_.ImportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedImportCount());
}

TEST_F(ObjectLinkerTest, CanImportWithDeadExportToken) {
  zx::eventpair export_token, import_token;
  {
    zx::eventpair export_token2;
    EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token2, &import_token));
    export_token = zx::eventpair{export_token2.get()};
    // |export_token2| dies now, |export_token| is an invalid copy.
  }

  TestImportObj import_obj(kImportValue);
  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(import_link.valid());
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());
}

TEST_F(ObjectLinkerTest, CannotImportSameTokenTwice) {
  zx::eventpair export_token, import_token, import_token2;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
  EXPECT_EQ(ZX_OK, import_token.duplicate(ZX_RIGHT_SAME_RIGHTS, &import_token2));

  TestImportObj import_obj(kImportValue);
  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(import_link.valid());
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

  TestObjectLinker::ImportLink import_link2 = object_linker_.CreateImport(
      std::move(import_obj), std::move(import_token2), error_reporter());
  EXPECT_ERROR_COUNT(1);  // CreateImport throws an error.
  EXPECT_FALSE(import_link2.valid());
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());
}

TEST_F(ObjectLinkerTest, LinkDeathRemovesImport) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));

  {
    TestImportObj import_obj(kImportValue);
    TestObjectLinker::ImportLink import_link = object_linker_.CreateImport(
        std::move(import_obj), std::move(import_token), error_reporter());
    EXPECT_ERROR_COUNT(0);
    EXPECT_TRUE(import_link.valid());
    EXPECT_EQ(1u, object_linker_.ImportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

    // | import_link dies now. |
  }

  EXPECT_EQ(0u, object_linker_.ImportCount());
}

// TODO(ES-179): Only fails in debug builds.
TEST_F(ObjectLinkerTest, DISABLED_InitializingLinkTwiceCausesDeath) {
  TestExportObj export_obj(kExportValue);
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));

  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(export_link.valid());

  export_link.Initialize(ERROR_IF_CALLED("export.link_resolved"),
                         ERROR_IF_CALLED("export.link_disconnected"));
  EXPECT_ERROR_COUNT(0);

  // 2nd Initialize() attempt dies with a DCHECK.
  EXPECT_DEATH_IF_SUPPORTED(export_link.Initialize(ERROR_IF_CALLED("export.link_resolved"),
                                                   ERROR_IF_CALLED("export.link_disconnected")),
                            "");
}

TEST_F(ObjectLinkerTest, InitializeLinksMatchingPeersExportBeforeImport) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
  TestExportObj export_obj(kExportValue);
  TestImportObj import_obj(kImportValue);
  bool export_linked = false, import_linked = false;
  bool export_disconnected = false, import_disconnected = false;
  bool export_fail_on_disconnect_called = false, import_fail_on_disconnect_called = false;

  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(export_link.valid());
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());

  {
    TestObjectLinker::ImportLink import_link = object_linker_.CreateImport(
        std::move(import_obj), std::move(import_token), error_reporter());
    EXPECT_ERROR_COUNT(0);
    EXPECT_TRUE(import_link.valid());
    EXPECT_EQ(1u, object_linker_.ImportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

    export_link.Initialize(
        [&import_linked](TestImportObj obj) {
          EXPECT_EQ(kImportValue, obj.value);
          EXPECT_FALSE(import_linked);
          import_linked = true;
        },
        [&export_fail_on_disconnect_called, &import_disconnected](bool on_link_destruction) {
          EXPECT_FALSE(on_link_destruction);
          EXPECT_FALSE(export_fail_on_disconnect_called);
          EXPECT_FALSE(import_disconnected);
          import_disconnected = true;
        });
    EXPECT_ERROR_COUNT(0);
    EXPECT_FALSE(export_linked);
    EXPECT_FALSE(import_linked);
    EXPECT_FALSE(export_disconnected);
    EXPECT_FALSE(import_disconnected);
    EXPECT_EQ(1u, object_linker_.ExportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());
    EXPECT_EQ(1u, object_linker_.ImportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

    import_link.Initialize(
        [&export_linked](TestExportObj obj) {
          EXPECT_EQ(kExportValue, obj.value);
          EXPECT_FALSE(export_linked);
          export_linked = true;
        },
        [&import_fail_on_disconnect_called, &export_disconnected](bool on_link_destruction) {
          EXPECT_TRUE(on_link_destruction);
          EXPECT_FALSE(import_fail_on_disconnect_called);
          EXPECT_FALSE(export_disconnected);
          export_disconnected = true;
        });
    EXPECT_ERROR_COUNT(0);
    EXPECT_TRUE(export_linked);
    EXPECT_TRUE(import_linked);
    EXPECT_FALSE(export_disconnected);
    EXPECT_FALSE(import_disconnected);
    EXPECT_EQ(1u, object_linker_.ExportCount());
    EXPECT_EQ(0u, object_linker_.UnresolvedExportCount());
    EXPECT_EQ(1u, object_linker_.ImportCount());
    EXPECT_EQ(0u, object_linker_.UnresolvedImportCount());

    // |import_link| dies now, which also invalidates |export_link|.
  }

  EXPECT_TRUE(import_disconnected);
  EXPECT_TRUE(export_disconnected);

  // |export_link| dies now. No additional disconnect callbacks should be called.
  export_fail_on_disconnect_called = true;
  import_fail_on_disconnect_called = true;
}

TEST_F(ObjectLinkerTest, InitializeLinksMatchingPeersImportBeforeExport) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
  TestExportObj export_obj(kExportValue);
  TestImportObj import_obj(kImportValue);
  bool export_linked = false, import_linked = false;
  bool export_disconnected = false, import_disconnected = false;
  bool export_fail_on_disconnect_called = false, import_fail_on_disconnect_called = false;

  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(import_link.valid());
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

  {
    import_link.Initialize(
        [&export_linked](TestExportObj obj) {
          EXPECT_EQ(kExportValue, obj.value);
          EXPECT_FALSE(export_linked);
          export_linked = true;
        },
        [&import_fail_on_disconnect_called, &export_disconnected](bool on_link_destruction) {
          EXPECT_FALSE(on_link_destruction);
          EXPECT_FALSE(import_fail_on_disconnect_called);
          EXPECT_FALSE(export_disconnected);
          export_disconnected = true;
        });
    EXPECT_ERROR_COUNT(0);
    EXPECT_FALSE(export_linked);
    EXPECT_FALSE(import_linked);
    EXPECT_FALSE(export_disconnected);
    EXPECT_FALSE(import_disconnected);
    EXPECT_EQ(1u, object_linker_.ImportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

    TestObjectLinker::ExportLink export_link = object_linker_.CreateExport(
        std::move(export_obj), std::move(export_token), error_reporter());
    EXPECT_ERROR_COUNT(0);
    EXPECT_TRUE(export_link.valid());
    EXPECT_FALSE(export_linked);
    EXPECT_FALSE(import_linked);
    EXPECT_FALSE(export_disconnected);
    EXPECT_FALSE(import_disconnected);
    EXPECT_EQ(1u, object_linker_.ExportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());
    EXPECT_EQ(1u, object_linker_.ImportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

    export_link.Initialize(
        [&import_linked](TestImportObj obj) {
          EXPECT_EQ(kImportValue, obj.value);
          EXPECT_FALSE(import_linked);
          import_linked = true;
        },
        [&export_fail_on_disconnect_called, &import_disconnected](bool on_link_destruction) {
          EXPECT_TRUE(on_link_destruction);
          EXPECT_FALSE(export_fail_on_disconnect_called);
          EXPECT_FALSE(import_disconnected);
          import_disconnected = true;
        });
    EXPECT_ERROR_COUNT(0);
    EXPECT_TRUE(export_linked);
    EXPECT_TRUE(import_linked);
    EXPECT_FALSE(export_disconnected);
    EXPECT_FALSE(import_disconnected);
    EXPECT_EQ(1u, object_linker_.ExportCount());
    EXPECT_EQ(0u, object_linker_.UnresolvedExportCount());
    EXPECT_EQ(1u, object_linker_.ImportCount());
    EXPECT_EQ(0u, object_linker_.UnresolvedImportCount());

    // |export_link| dies now, which also invalidates |import_link|.
  }

  EXPECT_TRUE(export_disconnected);
  EXPECT_TRUE(import_disconnected);

  // |import_link| dies now. No additional disconnect callbacks should be called.
  export_fail_on_disconnect_called = true;
  import_fail_on_disconnect_called = true;
}

TEST_F(ObjectLinkerTest, InitializeDoesNotLinkNonMatchingPeers) {
  bool export_disconnected = false, import_disconnected = false;
  bool export_fail_on_disconnect_called = false, import_fail_on_disconnect_called = false;
  {
    zx::eventpair export_token, import_token;
    zx::eventpair export_token2, import_token2;
    EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
    EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token2, &import_token2));
    TestExportObj export_obj(kExportValue);
    TestImportObj import_obj(kImportValue);

    TestObjectLinker::ImportLink import_link = object_linker_.CreateImport(
        std::move(import_obj), std::move(import_token), error_reporter());
    EXPECT_ERROR_COUNT(0);
    EXPECT_TRUE(import_link.valid());
    EXPECT_EQ(1u, object_linker_.ImportCount());
    EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

    {
      TestObjectLinker::ExportLink export_link = object_linker_.CreateExport(
          std::move(export_obj), std::move(export_token2), error_reporter());
      EXPECT_ERROR_COUNT(0);
      EXPECT_TRUE(export_link.valid());
      EXPECT_EQ(1u, object_linker_.ExportCount());
      EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());

      import_link.Initialize(
          ERROR_IF_CALLED("import.link_resolved"),
          [&import_fail_on_disconnect_called, &export_disconnected](bool on_link_destruction) {
            EXPECT_TRUE(on_link_destruction);
            EXPECT_FALSE(import_fail_on_disconnect_called);
            EXPECT_FALSE(export_disconnected);
            export_disconnected = true;
          });
      EXPECT_ERROR_COUNT(0);
      EXPECT_EQ(1u, object_linker_.ExportCount());
      EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());
      EXPECT_EQ(1u, object_linker_.ImportCount());
      EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

      export_link.Initialize(
          ERROR_IF_CALLED("export.link_resolved"),
          [&export_fail_on_disconnect_called, &import_disconnected](bool on_link_destruction) {
            EXPECT_TRUE(on_link_destruction);
            EXPECT_FALSE(export_fail_on_disconnect_called);
            EXPECT_FALSE(import_disconnected);
            import_disconnected = true;
          });
      EXPECT_ERROR_COUNT(0);
      EXPECT_EQ(1u, object_linker_.ExportCount());
      EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());
      EXPECT_EQ(1u, object_linker_.ImportCount());
      EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

      // |export_link| dies now. Only the |export_link| disconnected callback should be called.
    }

    EXPECT_TRUE(import_disconnected);
    EXPECT_FALSE(export_disconnected);

    // |import_link| dies now. Only the |import_link| disconnected callback should be called.
    export_fail_on_disconnect_called = true;
  }

  // Both links have died. No more disconnected callbacks should be called.
  EXPECT_TRUE(import_disconnected);
  EXPECT_TRUE(export_disconnected);

  import_fail_on_disconnect_called = true;
}

TEST_F(ObjectLinkerTest, EarlyImportTokenDeathCausesExportDisconnection) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
  TestExportObj export_obj(kExportValue);
  bool import_disconnected = false;

  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(export_link.valid());
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());

  // This should cause the export to get a link_disconnected event when it is
  // initialized.
  import_token.reset();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_FALSE(import_disconnected);
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());

  export_link.Initialize(ERROR_IF_CALLED("export.link_resolved"),
                         [&import_disconnected](bool on_link_destruction) {
                           EXPECT_FALSE(on_link_destruction);
                           EXPECT_FALSE(import_disconnected);
                           import_disconnected = true;
                         });
  EXPECT_FALSE(export_link.valid());
  EXPECT_TRUE(import_disconnected);
  EXPECT_EQ(0u, object_linker_.ExportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedExportCount());
}

TEST_F(ObjectLinkerTest, ImportTokenDeathCausesExportDisconnection) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
  TestExportObj export_obj(kExportValue);
  bool import_disconnected = false;

  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(export_link.valid());
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());

  export_link.Initialize(ERROR_IF_CALLED("export.link_resolved"),
                         [&import_disconnected](bool on_link_destruction) {
                           EXPECT_FALSE(on_link_destruction);
                           EXPECT_FALSE(import_disconnected);
                           import_disconnected = true;
                         });
  EXPECT_EQ(1u, object_linker_.ExportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedExportCount());

  // This should cause the export to get a link_disconnected event when the
  // eventloop ticks.
  import_token.reset();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_FALSE(export_link.valid());
  EXPECT_TRUE(import_disconnected);
  EXPECT_EQ(0u, object_linker_.ExportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedExportCount());
}

TEST_F(ObjectLinkerTest, EarlyExportTokenDeathCausesImportDisconnection) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
  TestImportObj import_obj(kImportValue);
  bool export_disconnected = false;

  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(import_link.valid());
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

  // This should cause the import to get a link_disconnected event when it is
  // initialized.
  export_token.reset();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_FALSE(export_disconnected);
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

  import_link.Initialize(ERROR_IF_CALLED("import.link_resolved"),
                         [&export_disconnected](bool on_link_destruction) {
                           EXPECT_FALSE(on_link_destruction);
                           EXPECT_FALSE(export_disconnected);
                           export_disconnected = true;
                         });
  EXPECT_FALSE(import_link.valid());
  EXPECT_TRUE(export_disconnected);
  EXPECT_EQ(0u, object_linker_.ImportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedImportCount());
}

TEST_F(ObjectLinkerTest, ExportTokenDeathCausesImportDisconnection) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));
  TestImportObj import_obj(kImportValue);
  bool export_disconnected = false;

  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  EXPECT_ERROR_COUNT(0);
  EXPECT_TRUE(import_link.valid());
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

  import_link.Initialize(ERROR_IF_CALLED("import.link_resolved"),
                         [&export_disconnected](bool on_link_destruction) {
                           EXPECT_FALSE(on_link_destruction);
                           EXPECT_FALSE(export_disconnected);
                           export_disconnected = true;
                         });
  EXPECT_EQ(1u, object_linker_.ImportCount());
  EXPECT_EQ(1u, object_linker_.UnresolvedImportCount());

  // This should cause the import to get a link_disconnected event when the
  // eventloop ticks.
  export_token.reset();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_FALSE(import_link.valid());
  EXPECT_TRUE(export_disconnected);
  EXPECT_EQ(0u, object_linker_.ImportCount());
  EXPECT_EQ(0u, object_linker_.UnresolvedImportCount());
}

TEST_F(ObjectLinkerTest, MoveInitializedLink) {
  zx::eventpair export_token, import_token;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token, &import_token));

  TestImportObj import_obj(kImportValue);
  TestExportObj export_obj(kExportValue);

  uint64_t import_linked = 0;
  uint64_t export_linked = 0;
  uint64_t import_disconnected = 0;
  uint64_t export_disconnected = 0;

  TestObjectLinker::ImportLink import_link =
      object_linker_.CreateImport(std::move(import_obj), std::move(import_token), error_reporter());
  import_link.Initialize([&](TestExportObj obj) { ++export_linked; },
                         [&](bool on_link_destruction) { ++import_disconnected; });

  TestObjectLinker::ExportLink export_link =
      object_linker_.CreateExport(std::move(export_obj), std::move(export_token), error_reporter());
  export_link.Initialize([&](TestImportObj obj) { ++import_linked; },
                         [&](bool on_link_destruction) { ++export_disconnected; });

  RunLoopUntilIdle();

  EXPECT_EQ(1u, import_linked);
  EXPECT_EQ(1u, export_linked);
  EXPECT_EQ(0u, import_disconnected);
  EXPECT_EQ(0u, export_disconnected);

  // Move the successful links into new objects.
  TestObjectLinker::ImportLink saved_import = std::move(import_link);
  TestObjectLinker::ExportLink saved_export = std::move(export_link);

  EXPECT_EQ(1u, import_linked);
  EXPECT_EQ(1u, export_linked);
  EXPECT_EQ(0u, import_disconnected);
  EXPECT_EQ(0u, export_disconnected);

  EXPECT_FALSE(import_link.valid());
  EXPECT_FALSE(export_link.valid());

  // Perform a second linking, re-using the stack variables that have been invalidated.
  zx::eventpair export_token2, import_token2;
  EXPECT_EQ(ZX_OK, zx::eventpair::create(0, &export_token2, &import_token2));
  TestImportObj import_obj2(kImportValue);
  TestExportObj export_obj2(kExportValue);

  uint64_t import_linked2 = 0;
  uint64_t export_linked2 = 0;
  uint64_t import_disconnected2 = 0;
  uint64_t export_disconnected2 = 0;

  import_link = object_linker_.CreateImport(std::move(import_obj2), std::move(import_token2),
                                            error_reporter());
  import_link.Initialize([&](TestExportObj obj) { ++export_linked2; },
                         [&](bool on_link_destruction) { ++import_disconnected2; });

  export_link = object_linker_.CreateExport(std::move(export_obj2), std::move(export_token2),
                                            error_reporter());
  export_link.Initialize([&](TestImportObj obj) { ++import_linked2; },
                         [&](bool on_link_destruction) { ++export_disconnected2; });

  RunLoopUntilIdle();

  // Confirm that linking has occurred.
  EXPECT_EQ(1u, import_linked2);
  EXPECT_EQ(1u, export_linked2);
  EXPECT_EQ(0u, import_disconnected2);
  EXPECT_EQ(0u, export_disconnected2);

  // Invalidate one the saved objects.
  saved_import = TestObjectLinker::ImportLink();

  // Confirm that both of the saved objects have been invalidated and that their disconnect
  // callbacks have been called.
  EXPECT_FALSE(saved_import.valid());
  EXPECT_FALSE(saved_export.valid());
  EXPECT_EQ(1u, import_disconnected);
  EXPECT_EQ(1u, export_disconnected);

  // Confirm that the new links have been untouched.
  EXPECT_TRUE(import_link.valid());
  EXPECT_TRUE(export_link.valid());
  EXPECT_EQ(0u, import_disconnected2);
  EXPECT_EQ(0u, export_disconnected2);

  // Invalidate the other saved object.
  saved_export = TestObjectLinker::ExportLink();

  // Confirm that nothing changes in the saved objects and that callbacks aren't called again.
  EXPECT_FALSE(saved_import.valid());
  EXPECT_FALSE(saved_export.valid());
  EXPECT_EQ(1u, import_disconnected);
  EXPECT_EQ(1u, export_disconnected);

  // Confirm that the new links are still untouched.
  EXPECT_TRUE(import_link.valid());
  EXPECT_TRUE(export_link.valid());
  EXPECT_EQ(0u, import_disconnected2);
  EXPECT_EQ(0u, export_disconnected2);

  // Invalidate in new links in the opposite order.
  export_link = TestObjectLinker::ExportLink();

  // Confirm that both new links are still invalidated and that their disconnect callbacks have
  // been called.
  EXPECT_FALSE(import_link.valid());
  EXPECT_FALSE(export_link.valid());
  EXPECT_EQ(1u, import_disconnected2);
  EXPECT_EQ(1u, export_disconnected2);

  // Invalidating the other link doesn't change anything.
  import_link = TestObjectLinker::ImportLink();
  EXPECT_FALSE(import_link.valid());
  EXPECT_FALSE(export_link.valid());
  EXPECT_EQ(1u, import_disconnected2);
  EXPECT_EQ(1u, export_disconnected2);
}

}  // namespace test
}  // namespace gfx
}  // namespace scenic_impl
