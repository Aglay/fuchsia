// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/hermetic-compute/hermetic-compute.h>
#include <lib/hermetic-compute/vmo-span.h>

#include <cerrno>
#include <cstring>
#include <fbl/unique_fd.h>
#include <fcntl.h>
#include <filesystem>
#include <lib/fdio/io.h>
#include <lib/zx/job.h>
#include <numeric>
#include <string>
#include <unistd.h>
#include <zxtest/zxtest.h>

#include "test-module-struct.h"

void GetElfVmo(std::filesystem::path module_path, zx::vmo* out_vmo) {
    {
        std::filesystem::path root_dir("/");
        if (const char* var = getenv("TEST_ROOT_DIR")) {
            root_dir = var;
        }
        module_path = root_dir / module_path;
    }

    fbl::unique_fd module_fd(open(module_path.c_str(), O_RDONLY));
    ASSERT_TRUE(module_fd.is_valid(), "cannot open %s: %s",
                module_path.c_str(), strerror(errno));

    ASSERT_OK(fdio_get_vmo_copy(module_fd.get(),
                                out_vmo->reset_and_get_address()));
    ASSERT_OK(out_vmo->replace_as_executable({}, out_vmo));
}

TEST(HermeticComputeTests, BasicModuleTest) {
    constexpr const char kTestModuleFile[] =
        "lib/hermetic/test-module-basic.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "basic-module-test"));

    // Synchronous load (default vDSO)
    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result, 17, 23));

    EXPECT_EQ(17 + 23, result);
}

TEST(HermeticComputeTests, ManyArgsTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-many-args.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "basic-module-test"));

    // This is enough arguments to require passing some on the stack.
    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result,
                       1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));

    EXPECT_EQ(1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12, result);
}

TEST(HermeticComputeTests, FloatTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-float.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "basic-module-test"));

    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result, 1.5f, 1.5, 1.5l));

    EXPECT_EQ(static_cast<int64_t>(1.5f + 1.5 + 1.5l), result);
}

TEST(HermeticComputeTests, PairTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-basic.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-pair-test"));

    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result, std::make_pair(17, 23)));

    EXPECT_EQ(17 + 23, result);
}

TEST(HermeticComputeTests, TupleTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-many-args.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-tuple-test"));

    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result,
                       std::make_tuple(1, 2, std::make_tuple(), 3, 4),
                       std::make_pair(5, std::make_tuple(6, 7, 8)),
                       std::make_tuple(std::make_tuple(9), 10,
                                       std::make_pair(11, 12))));

    EXPECT_EQ(1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12, result);
}

TEST(HermeticComputeTests, ArrayTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-many-args.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-array-test"));

    const std::array<std::tuple<int, int, int>, 4> array{{
            {1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12},
        }};
    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result, array));

    EXPECT_EQ(1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12, result);
}

TEST(HermeticComputeTests, DetupleTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-tuple.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-detuple-test"));

    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result,
                       1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));

    EXPECT_EQ(1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12, result);
}

TEST(HermeticComputeTests, StructTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-struct.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-struct-test"));

    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result,
                       OneWord{1}, MultiWord{2, 3, 4}, Tiny{5, 6}, MakeOdd()));

    EXPECT_EQ(1 + 2 + 3 + 4 + 5 + 6 + MakeOdd().Total(), result);
}

struct FailToExport {};

template <>
class HermeticExportAgent<FailToExport> :
    public HermeticExportAgentBase<FailToExport> {
public:
    using Base = HermeticExportAgentBase<FailToExport>;
    using type = typename Base::type;
    explicit HermeticExportAgent(
        HermeticComputeProcess::Launcher& launcher) : Base(launcher) {}

    auto operator()(FailToExport) {
        launcher().Abort(ZX_ERR_UNAVAILABLE);
        return std::make_tuple();
    }
};

TEST(HermeticComputeTests, HermeticExportAgentAbortTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-basic.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-agent-abort-test"));

    int64_t result;
    EXPECT_EQ(ZX_ERR_UNAVAILABLE,
              hcp.Call(module_elf_vmo, {}, &result, FailToExport{}));
}

TEST(HermeticComputeTests, VmoSpanTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-vmo.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-vmo-test"));

    // Make a VMO and put some data in it.
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(PAGE_SIZE, 0, &vmo));
    const uint8_t data[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    ASSERT_OK(vmo.write(data, 0, sizeof(data)));

    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result,
                       VmoSpan{vmo, 0, PAGE_SIZE}));

    EXPECT_EQ(std::accumulate(std::begin(data), std::end(data), 0), result);
}

TEST(HermeticComputeTests, WritableVmoSpanTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-vmo-out.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-vmo-out-test"));

    constexpr size_t kSize = 456;
    constexpr uint8_t kValue = 42;

    // Make a VMO where the engine will deliver data.
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(PAGE_SIZE, 0, &vmo));
    static_assert(kSize <= PAGE_SIZE);

    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result,
                       WritableVmoSpan{vmo, 0, PAGE_SIZE}));

    // Read back the data.
    uint8_t data[kSize];
    ASSERT_OK(vmo.read(data, 0, kSize));

    // Check that every byte holds the answer.
    EXPECT_TRUE(std::all_of(std::begin(data), std::end(data),
                            [](uint8_t x) { return x == kValue; }));
}

TEST(HermeticComputeTests, LeakyVmoSpanTest) {
    constexpr const char kTestModuleFile[]
        = "lib/hermetic/test-module-vmo.so";
    zx::vmo module_elf_vmo;
    ASSERT_NO_FATAL_FAILURES(GetElfVmo(kTestModuleFile, &module_elf_vmo),
                             "loading %s", kTestModuleFile);

    HermeticComputeProcess hcp;
    ASSERT_OK(hcp.Init(*zx::job::default_job(), "hermetic-vmo-leaky-test"));

    // Make a VMO and put some data in it.
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(PAGE_SIZE, 0, &vmo));
    const uint8_t data[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    constexpr uint64_t kOffset = 128;
    static_assert(kOffset % PAGE_SIZE != 0);
    ASSERT_OK(vmo.write(data, kOffset, sizeof(data)));

    int64_t result;
    ASSERT_OK(hcp.Call(module_elf_vmo, {}, &result,
                       LeakyVmoSpan{vmo, kOffset, sizeof(data)}));

    EXPECT_EQ(std::accumulate(std::begin(data), std::end(data), 0), result);
}
