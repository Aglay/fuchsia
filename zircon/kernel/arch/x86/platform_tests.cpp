// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <arch/arch_ops.h>
#include <arch/mp.h>
#include <arch/x86.h>
#include <arch/x86/cpuid.h>
#include <arch/x86/cpuid_test_data.h>
#include <arch/x86/feature.h>
#include <arch/x86/platform_access.h>
#include <ktl/array.h>
#include <lib/console.h>
#include <lib/unittest/unittest.h>

static bool test_x64_msrs() {
    BEGIN_TEST;

    arch_disable_ints();
    // Test read_msr for an MSR that is known to always exist on x64.
    uint64_t val = read_msr(X86_MSR_IA32_LSTAR);
    EXPECT_NE(val, 0ull, "");

    // Test write_msr to write that value back.
    write_msr(X86_MSR_IA32_LSTAR, val);
    arch_enable_ints();

    // Test read_msr_safe for an MSR that is known to not exist.
    // If read_msr_safe is busted, then this will #GP (panic).
    // TODO: Enable when the QEMU TCG issue is sorted (TCG never
    // generates a #GP on MSR access).
#ifdef DISABLED
    uint64_t bad_val;
    // AMD MSRC001_2xxx are only readable via Processor Debug.
    auto bad_status = read_msr_safe(0xC0012000, &bad_val);
    EXPECT_NE(bad_status, ZX_OK, "");
#endif

    // Test read_msr_on_cpu.
    uint64_t initial_fmask = read_msr(X86_MSR_IA32_FMASK);
    for (uint i = 0; i < arch_max_num_cpus(); i++) {
        if (!mp_is_cpu_online(i)) {
            continue;
        }
        uint64_t fmask = read_msr_on_cpu(/*cpu=*/i, X86_MSR_IA32_FMASK);
        EXPECT_EQ(initial_fmask, fmask, "");
    }

    // Test write_msr_on_cpu
    for (uint i = 0; i < arch_max_num_cpus(); i++) {
        if (!mp_is_cpu_online(i)) {
            continue;
        }
        write_msr_on_cpu(/*cpu=*/i, X86_MSR_IA32_FMASK, /*val=*/initial_fmask);
    }

    END_TEST;
}

static bool test_x64_msrs_k_commands() {
    BEGIN_TEST;

    console_run_script_locked("cpu rdmsr 0 0x10");

    END_TEST;
}

class FakeMsrAccess : public MsrAccess {
  public:
    struct FakeMsr {
        uint32_t index;
        uint64_t value;
    };

    uint64_t read_msr(uint32_t msr_index) override {
        for (uint i = 0; i < msrs_.size(); i++) {
            if (msrs_[i].index == msr_index) {
                return msrs_[i].value;
            }
        }
        DEBUG_ASSERT(0);  // Unexpected MSR read
        return 0;
    }

    ktl::array<FakeMsr, 1> msrs_;
};

static bool test_x64_mds_enumeration() {
    BEGIN_TEST;

    {
        // Test an Intel Xeon E5-2690 V4 w/ older microcode (no ARCH_CAPABILITIES)
        FakeMsrAccess fake_msrs;
        EXPECT_TRUE(x86_intel_cpu_has_mds(&cpu_id::kCpuIdXeon2690v4, &fake_msrs), "");
    }

    {
        // Test an Intel Xeon E5-2690 V4 w/ new microcode (ARCH_CAPABILITIES available)
        cpu_id::TestDataSet data = cpu_id::kTestDataXeon2690v4;
        data.leaf7.reg[cpu_id::Features::ARCH_CAPABILITIES.reg] |=
            (1 << cpu_id::Features::ARCH_CAPABILITIES.bit);
        cpu_id::FakeCpuId cpu(data);
        FakeMsrAccess fake_msrs = {};
        fake_msrs.msrs_[0] = { X86_MSR_IA32_ARCH_CAPABILITIES, 0 };
        EXPECT_TRUE(x86_intel_cpu_has_mds(&cpu, &fake_msrs), "");
    }

    {
        // Intel(R) Xeon(R) Gold 6xxx; does not have MDS
        cpu_id::TestDataSet data = {};
        data.leaf0 = { .reg = { 0x16, 0x756e6547, 0x6c65746e, 0x49656e69 } };
        data.leaf1 = { .reg = { 0x50656, 0x12400800, 0x7ffefbff, 0xbfebfbff } };
        data.leaf4 = { .reg = { 0x7c004121, 0x1c0003f, 0x3f, 0x0 } };
        data.leaf7 = { .reg = { 0x0, 0xd39ffffb, 0x808, 0xbc000400 } };

        cpu_id::FakeCpuId cpu(data);
        FakeMsrAccess fake_msrs = {};
        fake_msrs.msrs_[0] = { X86_MSR_IA32_ARCH_CAPABILITIES, 0x2b };
        EXPECT_FALSE(x86_intel_cpu_has_mds(&cpu, &fake_msrs), "");
    }

    {
        // Intel(R) Celeron(R) CPU J3455 (Goldmont) does not have MDS but does not
        // enumerate MDS_NO with microcode 32h (at least)
        cpu_id::TestDataSet data = {};
        data.leaf0 = { .reg = { 0x15, 0x756e6547, 0x6c65746e, 0x49656e69 } };
        data.leaf1 = { .reg = { 0x506c9, 0x2200800, 0x4ff8ebbf, 0xbfebfbff } };
        data.leaf4 = { .reg = { 0x3c000121, 0x140003f, 0x3f, 0x1 } };
        data.leaf7 = { .reg = { 0x0, 0x2294e283, 0x0, 0x2c000000 } };

        cpu_id::FakeCpuId cpu(data);
        FakeMsrAccess fake_msrs = {};
        // 0x19 = RDCL_NO | SKIP_VMENTRY_L1DFLUSH | SSB_NO
        fake_msrs.msrs_[0] = { X86_MSR_IA32_ARCH_CAPABILITIES, 0x19 };
        EXPECT_FALSE(x86_intel_cpu_has_mds(&cpu, &fake_msrs), "");
    }

    END_TEST;
}

UNITTEST_START_TESTCASE(x64_platform_tests)
UNITTEST("basic test of read/write MSR variants", test_x64_msrs)
UNITTEST("test k cpu rdmsr commands", test_x64_msrs_k_commands)
UNITTEST("test enumeration of x64 MDS vulnerability", test_x64_mds_enumeration)
UNITTEST_END_TESTCASE(x64_platform_tests, "x64_platform_tests", "");
