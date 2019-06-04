// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/auto_call.h>
#include <fbl/function.h>
#include <lib/zx/bti.h>
#include <lib/zx/iommu.h>
#include <lib/zx/pager.h>
#include <lib/zx/port.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <unittest/unittest.h>
#include <zircon/assert.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/iommu.h>

extern "C" __WEAK zx_handle_t get_root_resource(void);

namespace {

bool vmo_write(const zx::vmo& vmo, uint32_t data, uint32_t offset = 0) {
    zx_status_t status = vmo.write(static_cast<void*>(&data), offset, sizeof(data));
    if (status != ZX_OK) {
        unittest_printf_critical(" write failed %d", status);
        return false;
    }
    return true;
}

bool vmo_check(const zx::vmo& vmo, uint32_t expected, uint32_t offset = 0) {
    uint32_t data;
    zx_status_t status = vmo.read(static_cast<void*>(&data), offset, sizeof(data));
    if (status != ZX_OK) {
        unittest_printf_critical(" read failed %d", status);
        return false;
    }
    if (data != expected) {
        unittest_printf_critical(" got %u expected %u", data, expected);
        return false;
    }
    return true;
}

// Creates a vmo with |page_count| pages and writes (page_index + 1) to each page.
bool init_page_tagged_vmo(uint32_t page_count, zx::vmo* vmo) {
    zx_status_t status;
    status = zx::vmo::create(page_count * ZX_PAGE_SIZE, ZX_VMO_RESIZABLE, vmo);
    if (status != ZX_OK) {
        unittest_printf_critical(" create failed %d", status);
        return false;
    }
    for (unsigned i = 0; i < page_count; i++) {
        if (!vmo_write(*vmo, i + 1, i * ZX_PAGE_SIZE)) {
            return false;
        }
    }
    return true;
}

size_t vmo_num_children(const zx::vmo& vmo) {
    zx_info_vmo_t info;
    if (vmo.get_info(ZX_INFO_VMO, &info, sizeof(info), nullptr, nullptr) != ZX_OK) {
        return UINT64_MAX;
    }
    return info.num_children;
}

size_t vmo_committed_bytes(const zx::vmo& vmo) {
    zx_info_vmo_t info;
    if (vmo.get_info(ZX_INFO_VMO, &info, sizeof(info), nullptr, nullptr) != ZX_OK) {
        return UINT64_MAX;
    }
    return info.committed_bytes;
}

size_t kmem_vmo_mem_usage() {
    zx_info_kmem_stats_t info;
    if (zx::unowned_resource(get_root_resource())->get_info(
                ZX_INFO_KMEM_STATS, &info, sizeof(info), nullptr, nullptr) != ZX_OK) {
        return UINT64_MAX;
    }
    return info.vmo_bytes;
}

// Simple class for managing vmo mappings w/o any external dependencies.
class Mapping {
public:
    ~Mapping() {
        if (addr_) {
            ZX_ASSERT(zx::vmar::root_self()->unmap(addr_, len_) == ZX_OK);
        }
    }

    zx_status_t Init(const zx::vmo& vmo, size_t len) {
        zx_status_t status = zx::vmar::root_self()->map(0, vmo, 0, len,
                                                        ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, &addr_);
        len_ = len;
        return status;
    }

    uint32_t* ptr() { return reinterpret_cast<uint32_t*>(addr_); }

private:
    uint64_t addr_ = 0;
    size_t len_ = 0;
};

// Helper function which checks that the give vmo is contiguous.
template<size_t N>
bool check_contig_state(const zx::bti& bti, const zx::vmo& vmo) {
    zx::pmt pmt;
    zx_paddr_t addrs[N];
    zx_status_t status = bti.pin(ZX_BTI_PERM_READ, vmo, 0, N * ZX_PAGE_SIZE, addrs, N, &pmt);
    if (status != ZX_OK) {
        unittest_printf_critical(" pin failed %d", status);
        return false;
    }
    pmt.unpin();

    for (unsigned i = 0; i < N - 1; i++) {
        if (addrs[i] + ZX_PAGE_SIZE != addrs[i + 1]) {
            return false;
        }
    }

    return true;
}

// Helper function for call_permutations
template <typename T>
bool call_permutations_helper(T fn, uint32_t count,
                                     uint32_t perm[], bool elts[], uint32_t idx) {
    if (idx == count) {
        return fn(perm);
    }
    for (unsigned i = 0; i < count; i++) {
        if (elts[i]) {
            continue;
        }

        elts[i] = true;
        perm[idx] = i;

        if (!call_permutations_helper(fn, count, perm, elts, idx + 1)) {
            return false;
        }

        elts[i] = false;
    }
    return true;
}

// Function which invokes |fn| with all the permutations of [0...count-1].
template <typename T>
bool call_permutations(T fn, uint32_t count) {
    uint32_t perm[count];
    bool elts[count];

    for (unsigned i = 0; i < count; i++) {
        perm[i] = 0;
        elts[i] = false;
    }

    return call_permutations_helper(fn, count, perm, elts, 0);
}

// Checks the correctness of various zx_info_vmo_t properties.
bool info_test() {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    zx_info_vmo_t orig_info;
    if (vmo.get_info(ZX_INFO_VMO, &orig_info, sizeof(orig_info), nullptr, nullptr) != ZX_OK) {
        return UINT64_MAX;
    }

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone), ZX_OK);

    zx_info_vmo_t new_info;
    if (vmo.get_info(ZX_INFO_VMO, &new_info, sizeof(new_info), nullptr, nullptr) != ZX_OK) {
        return UINT64_MAX;
    }

    zx_info_vmo_t clone_info;
    if (clone.get_info(ZX_INFO_VMO, &clone_info, sizeof(clone_info), nullptr, nullptr) != ZX_OK) {
        return UINT64_MAX;
    }

    // Check for consistency of koids.
    ASSERT_EQ(orig_info.koid, new_info.koid);
    ASSERT_NE(orig_info.koid, clone_info.koid);
    ASSERT_EQ(clone_info.parent_koid, orig_info.koid);

    // Check that flags are properly set.
    constexpr uint32_t kOriginalFlags = ZX_INFO_VMO_TYPE_PAGED | ZX_INFO_VMO_VIA_HANDLE;
    constexpr uint32_t kCloneFlags =
            ZX_INFO_VMO_TYPE_PAGED | ZX_INFO_VMO_IS_COW_CLONE | ZX_INFO_VMO_VIA_HANDLE;
    ASSERT_EQ(orig_info.flags, kOriginalFlags);
    ASSERT_EQ(new_info.flags, kOriginalFlags);
    ASSERT_EQ(clone_info.flags, kCloneFlags);

    END_TEST;
}

// Tests that reading from a clone gets the correct data.
bool read_test() {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    ASSERT_TRUE(vmo_write(vmo, kOriginalData));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone), ZX_OK);

    ASSERT_TRUE(vmo_check(vmo, kOriginalData));
    ASSERT_TRUE(vmo_check(clone, kOriginalData));

    END_TEST;
}

// Tests that zx_vmo_write into the (clone|parent) doesn't affect the other.
bool vmo_write_test(bool clone_write) {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    static constexpr uint32_t kNewData = 0xc0ffee;
    ASSERT_TRUE(vmo_write(vmo, kOriginalData));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone), ZX_OK);

    ASSERT_TRUE(vmo_write(clone_write ? clone : vmo, kNewData));

    ASSERT_TRUE(vmo_check(vmo, clone_write ? kOriginalData : kNewData));
    ASSERT_TRUE(vmo_check(clone, clone_write ? kNewData : kOriginalData));

    END_TEST;
}

bool clone_vmo_write_test() {
    return vmo_write_test(true);
}

bool parent_vmo_write_test() {
    return vmo_write_test(false);
}

// Tests that writing into the mapped (clone|parent) doesn't affect the other.
bool vmar_write_test(bool clone_write) {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    Mapping vmo_mapping;
    ASSERT_EQ(vmo_mapping.Init(vmo, ZX_PAGE_SIZE), ZX_OK);

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    static constexpr uint32_t kNewData = 0xc0ffee;
    *vmo_mapping.ptr() = kOriginalData;

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone), ZX_OK);

    Mapping clone_mapping;
    ASSERT_EQ(clone_mapping.Init(clone, ZX_PAGE_SIZE), ZX_OK);

    *(clone_write ? clone_mapping.ptr(): vmo_mapping.ptr()) =  kNewData;

    ASSERT_EQ(*vmo_mapping.ptr(), clone_write ? kOriginalData : kNewData);
    ASSERT_EQ(*clone_mapping.ptr(), clone_write ? kNewData : kOriginalData);

    END_TEST;
}

bool clone_vmar_write_test() {
    return vmar_write_test(true);
}

bool parent_vmar_write_test() {
    return vmar_write_test(false);
}

// Tests that closing the (parent|clone) doesn't affect the other.
bool close_test(bool close_orig) {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    ASSERT_TRUE(vmo_write(vmo, kOriginalData));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone), ZX_OK);

    (close_orig ? vmo : clone).reset();

    ASSERT_TRUE(vmo_check(close_orig ? clone : vmo, kOriginalData));

    END_TEST;
}

bool close_original_test() {
    return close_test(true);
}

bool close_clone_test() {
    return close_test(false);
}

// Basic memory accounting test that checks vmo memory attribution.
bool obj_mem_accounting_test() {
    BEGIN_TEST;

    // Create a vmo, write to both pages, and check the committed stats.
    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(2 * ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    ASSERT_TRUE(vmo_write(vmo, 1, 0));
    ASSERT_TRUE(vmo_write(vmo, 1, ZX_PAGE_SIZE));

    ASSERT_EQ(vmo_committed_bytes(vmo), 2 * ZX_PAGE_SIZE);

    // Create a clone and check the initialze committed stats.
    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone), ZX_OK);

    ASSERT_EQ(vmo_committed_bytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), 0);

    // Write to the clone and check that that forks a page into the clone.
    ASSERT_TRUE(vmo_write(vmo, 2, 0));
    ASSERT_EQ(vmo_committed_bytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), ZX_PAGE_SIZE);

    // Write to the orignal and check that that forks a page into the clone.
    ASSERT_TRUE(vmo_write(clone, 2, ZX_PAGE_SIZE));
    ASSERT_EQ(vmo_committed_bytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), 2 * ZX_PAGE_SIZE);

    // Write to the other pages, which shouldn't affect accounting.
    ASSERT_TRUE(vmo_write(vmo, 2, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_write(clone, 2, 0));
    ASSERT_EQ(vmo_committed_bytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), 2 * ZX_PAGE_SIZE);

    END_TEST;
}

// Basic memory accounting test that total memory consumption through kmem.
bool kmem_accounting_test() {
    BEGIN_TEST;

    if (!get_root_resource) {
        unittest_printf_critical(" Root resource not available, skipping");
        return true;
    }

    uint64_t start_size = kmem_vmo_mem_usage();

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(2 * ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    // An new vmo consumes no pages
    ASSERT_EQ(start_size, kmem_vmo_mem_usage());

    ASSERT_TRUE(vmo_write(vmo, 1, 0));
    ASSERT_TRUE(vmo_write(vmo, 1, ZX_PAGE_SIZE));

    // Check that the two pages were comitted.
    ASSERT_EQ(start_size + 2 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone), ZX_OK);

    // A clone shouldn't allocate more pages.
    ASSERT_EQ(start_size + 2 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());

    // Forking a page through the original should allocate a page
    ASSERT_TRUE(vmo_write(vmo, 2, 0));
    ASSERT_EQ(start_size + 3 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());

    // Forking a page through the clone should allocate a page.
    ASSERT_TRUE(vmo_write(clone, 2, ZX_PAGE_SIZE));
    ASSERT_EQ(start_size + 4 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());

    // Writing to already-forked pages shouldn't allocate anything
    ASSERT_TRUE(vmo_write(vmo, 2, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_write(clone, 2, 0));
    ASSERT_EQ(start_size + 4 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());

    // Make sure pages are properly freed on close.
    vmo.reset();
    ASSERT_EQ(start_size + 2 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());

    clone.reset();
    ASSERT_EQ(start_size, kmem_vmo_mem_usage());

    END_TEST;
}


// Tests that writes to a COW'ed zero page work and don't require redundant allocations.
bool zero_page_write_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmos[4];
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, vmos), ZX_OK);

    // Create two clones of the original vmo and one clone of one of those clones.
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 1), ZX_OK);
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 2), ZX_OK);
    ASSERT_EQ(vmos[1].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 3), ZX_OK);

    if (original) {
        ASSERT_EQ(original, kmem_vmo_mem_usage());
    }

    for (unsigned i = 0; i < 4; i++) {
        ASSERT_TRUE(vmo_write(vmos[i], i + 1));
        for (unsigned j = 0; j < 4; j++) {
            ASSERT_TRUE(vmo_check(vmos[j], j <= i ? j + 1 : 0));
            ASSERT_EQ(vmo_committed_bytes(vmos[j]), (j <= i ? 1u : 0u) * ZX_PAGE_SIZE);
        }
        if (original) {
            ASSERT_EQ(original + (i + 1) * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
        }
    }

    END_TEST;
}

// Tests that a clone with an offset accesses the right data and doesn't
// unnecessarily retain pages when the parent is closed.
bool offset_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(3, &vmo));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               ZX_PAGE_SIZE, 3 * ZX_PAGE_SIZE, &clone), ZX_OK);

    // Check that the child has the right data.
    ASSERT_TRUE(vmo_check(clone, 2));
    ASSERT_TRUE(vmo_check(clone, 3, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone, 0, 2 * ZX_PAGE_SIZE));

    ASSERT_TRUE(vmo_write(clone, 4, ZX_PAGE_SIZE));

    vmo.reset();

    // Check that we don't change the child.
    ASSERT_TRUE(vmo_check(clone, 2));
    ASSERT_TRUE(vmo_check(clone, 4, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone, 0, 2 * ZX_PAGE_SIZE));

    // Check that the clone doesn't unnecessarily retain pages.
    ASSERT_EQ(vmo_committed_bytes(clone), 2 * ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + 2 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

// Tests that a clone of a clone which overflows its parent properly interacts with
// both of its ancestors (i.e. the orignal vmo and the first clone).
bool overflow_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(1, &vmo));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone), ZX_OK);

    // Check that the child has the right data.
    ASSERT_TRUE(vmo_check(clone, 1));
    ASSERT_TRUE(vmo_check(clone, 0, ZX_PAGE_SIZE));

    // Write to the child and then clone it.
    ASSERT_TRUE(vmo_write(clone, 2, ZX_PAGE_SIZE));
    zx::vmo clone2;
    ASSERT_EQ(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 3 * ZX_PAGE_SIZE, &clone2), ZX_OK);

    // Check that the second clone is correct.
    ASSERT_TRUE(vmo_check(clone2, 1));
    ASSERT_TRUE(vmo_check(clone2, 2, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone2, 0, 2 * ZX_PAGE_SIZE));

    // Write the dedicated page in 2nd child and then check that accounting is correct.
    ASSERT_TRUE(vmo_write(clone2, 3, 2 * ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone2, 3, 2 * ZX_PAGE_SIZE));

    // Check that accounting is correct.
    ASSERT_EQ(vmo_committed_bytes(vmo), ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone2), ZX_PAGE_SIZE);

    if (get_root_resource) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    // Completely fork the final clone and check that things are correct.
    ASSERT_TRUE(vmo_write(clone2, 4, 0));
    ASSERT_TRUE(vmo_write(clone2, 5, ZX_PAGE_SIZE));

    ASSERT_TRUE(vmo_check(vmo, 1, 0));
    ASSERT_TRUE(vmo_check(clone, 1, 0));
    ASSERT_TRUE(vmo_check(clone, 2, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone2, 4, 0));
    ASSERT_TRUE(vmo_check(clone2, 5, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone2, 3, 2 * ZX_PAGE_SIZE));

    // Check that the total amount of allocated memory is correct. The amount allocated
    // is implementation dependent, but no vmo can be blamed for more pages than its total size.
    constexpr uint64_t kImplCost1 = ZX_PAGE_SIZE;
    constexpr uint64_t kImplCost2 = 2 * ZX_PAGE_SIZE;
    constexpr uint64_t kImplCost3 = 3 * ZX_PAGE_SIZE;
    static_assert(kImplCost1 <= ZX_PAGE_SIZE && kImplCost2 <= 2 * ZX_PAGE_SIZE
            && kImplCost3 <= 3 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(vmo), kImplCost1);
    ASSERT_EQ(vmo_committed_bytes(clone), kImplCost2);
    ASSERT_EQ(vmo_committed_bytes(clone2), kImplCost3);
    if (get_root_resource) {
        ASSERT_EQ(original + kImplCost1 + kImplCost2 + kImplCost3, kmem_vmo_mem_usage());
    }

    // Close the middle clone and check that things are still correct. Memory usage
    // between the two vmos is not implementation dependent.
    clone.reset();

    ASSERT_TRUE(vmo_check(vmo, 1, 0));
    ASSERT_TRUE(vmo_check(clone2, 4));
    ASSERT_TRUE(vmo_check(clone2, 5, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone2, 3, 2 * ZX_PAGE_SIZE));

    ASSERT_EQ(vmo_committed_bytes(vmo), ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone2), 3 * ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

// Tests that a small clone doesn't require allocations for pages which it doesn't
// have access to and that unneeded pages get freed if the original vmo is closed.
bool small_clone_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(3, &vmo));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone), ZX_OK);

    // Check that the child has the right data.
    ASSERT_TRUE(vmo_check(clone, 2));

    // Check that a write into the original vmo out of bounds of the first clone
    // doesn't allocate any memory.
    ASSERT_TRUE(vmo_write(vmo, 4, 0));
    ASSERT_TRUE(vmo_write(vmo, 5, 2 * ZX_PAGE_SIZE));
    ASSERT_EQ(vmo_committed_bytes(vmo), 3 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), 0);
    if (get_root_resource) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    vmo.reset();

    // Check that clone has the right data after closing the parent and that
    // all the extra pages are freed.
    ASSERT_TRUE(vmo_check(clone, 2));
    ASSERT_EQ(vmo_committed_bytes(clone), ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

// Tests that a small clone properly interrupts access into the parent.
bool small_clone_child_test() {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(3, &vmo));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone), ZX_OK);

    // Check that the child has the right data.
    ASSERT_TRUE(vmo_check(clone, 2));

    // Create a clone of the first clone and check that it has the right data (incl. that
    // it can't access the original vmo).
    zx::vmo clone2;
    ASSERT_EQ(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone2), ZX_OK);
    ASSERT_TRUE(vmo_check(clone2, 2));
    ASSERT_TRUE(vmo_check(clone2, 0, ZX_PAGE_SIZE));

    END_TEST;
}

// Tests that closing a vmo with multiple small clones properly frees pages.
bool small_clones_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(3, &vmo));

    // Create a clone and populate one of its pages
    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone), ZX_OK);
    ASSERT_TRUE(vmo_write(clone, 4, ZX_PAGE_SIZE));

    // Create a second clone
    zx::vmo clone2;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 1 * ZX_PAGE_SIZE, &clone2), ZX_OK);

    ASSERT_EQ(vmo_committed_bytes(vmo), 3 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone2), 0);
    if (get_root_resource) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    vmo.reset();

    // The inaccessible 3rd page should be freed, and vmo's copy of page 2 should be freed. The
    // fact that both are blamed to clone (vs 1 being blamed to clone2) is implementation
    // dependent.
    constexpr uint64_t kImplClone1Cost = 2 * ZX_PAGE_SIZE;
    constexpr uint64_t kImplClone2Cost = 0;
    static_assert(kImplClone1Cost <= 2 * ZX_PAGE_SIZE && kImplClone2Cost <= ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), kImplClone1Cost);
    ASSERT_EQ(vmo_committed_bytes(clone2), kImplClone2Cost);
    if (get_root_resource) {
        ASSERT_EQ(original + 2 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

// Tests that disjoint clones work (i.e. create multiple clones, none of which overlap) and that
// they don't unnecessarily retain/allocate memory after closing the original VMO. This tests
// two cases - resetting the original vmo before writing to the clones and resetting the original
// vmo after writing to the clones.
bool disjoint_clone_test(bool early_close) {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(4, &vmo));

    // Create a disjoint clone for each page in the orignal vmo: 2 direct and 2 through another
    // intermediate COW clone.
    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               1 * ZX_PAGE_SIZE, 2 * ZX_PAGE_SIZE, &clone), ZX_OK);

    zx::vmo leaf_clones[4];
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, leaf_clones), ZX_OK);
    ASSERT_EQ(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, leaf_clones + 1),
              ZX_OK);
    ASSERT_EQ(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                 ZX_PAGE_SIZE, ZX_PAGE_SIZE, leaf_clones + 2), ZX_OK);
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                        3 * ZX_PAGE_SIZE, ZX_PAGE_SIZE, leaf_clones + 3), ZX_OK);

    if (early_close) {
        vmo.reset();
        clone.reset();
    }

    // Check that each clone's has the correct data and then write to the clone.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_TRUE(vmo_check(leaf_clones[i], i + 1));
        ASSERT_TRUE(vmo_write(leaf_clones[i], i + 5));
    }

    if (!early_close) {
        // The number of allocated pages is implementation dependent, but it must be less
        // than the total user-visible vmo size.
        constexpr uint32_t kImplTotalPages = 10;
        static_assert(kImplTotalPages <= 10);
        if (original) {
            ASSERT_EQ(original + 10 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
        }
        vmo.reset();
        clone.reset();
    }

    // Check that the clones have the correct data and that nothing
    // is unnecessary retained/allocated.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_TRUE(vmo_check(leaf_clones[i], i + 5));
        ASSERT_EQ(vmo_committed_bytes(leaf_clones[i]), ZX_PAGE_SIZE);
    }
    if (original) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

bool disjoint_clone_early_close_test() {
    return disjoint_clone_test(true);
}

bool disjoint_clone_late_close_test() {
    return disjoint_clone_test(false);
}

// A second disjoint clone test that checks that closing the disjoint clones which haven't
// yet been written to doesn't affect the contents of other disjoint clones.
bool disjoint_clone_test2() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    auto test_fn = [original](uint32_t perm[]) -> bool {
        zx::vmo vmo;
        ASSERT_TRUE(init_page_tagged_vmo(4, &vmo));

        // Create a disjoint clone for each page in the orignal vmo: 2 direct and 2 through another
        // intermediate COW clone.
        zx::vmo clone;
        ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   1 * ZX_PAGE_SIZE, 2 * ZX_PAGE_SIZE, &clone), ZX_OK);

        zx::vmo leaf_clones[4];
        ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   0, ZX_PAGE_SIZE, leaf_clones), ZX_OK);
        ASSERT_EQ(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                     0, ZX_PAGE_SIZE, leaf_clones + 1), ZX_OK);
        ASSERT_EQ(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                     ZX_PAGE_SIZE, ZX_PAGE_SIZE, leaf_clones + 2), ZX_OK);
        ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   3 * ZX_PAGE_SIZE, ZX_PAGE_SIZE, leaf_clones + 3), ZX_OK);

        vmo.reset();
        clone.reset();

        // Check that each clone's has the correct data and then write to the clone.
        for (unsigned i = 0; i < 4; i++) {
            ASSERT_TRUE(vmo_check(leaf_clones[i], i + 1));
        }

        // Nothing should have been allocated by the writes.
        if (original) {
            ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
        }

        // Close the clones in the order specified by |perm|, and at each step
        // check the rest of the clones.
        bool closed[4] = {};
        for (unsigned i = 0; i < 4; i++) {
            leaf_clones[perm[i]].reset();
            closed[perm[i]] = true;

            for (unsigned j = 0; j < 4; j++) {
                if (!closed[j]) {
                    ASSERT_TRUE(vmo_check(leaf_clones[j], j + 1));
                    ASSERT_EQ(vmo_committed_bytes(leaf_clones[j]), ZX_PAGE_SIZE);
                }
            }
            if (original) {
                ASSERT_EQ(original + (3 - i) * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
            }
        }

        return true;
    };

    ASSERT_TRUE(call_permutations(test_fn, 4));

    END_TEST;
}

enum class Contiguity {
    Contig,
    NonContig,
};

enum class ResizeTarget {
    Parent,
    Child,
};

// Tests that resizing a (clone|cloned) vmo frees unnecessary pages.
bool resize_test(Contiguity contiguity, ResizeTarget target) {
    BEGIN_TEST;

    bool contiguous = contiguity == Contiguity::Contig;
    bool resize_child = target == ResizeTarget::Child;

    if (contiguous && !get_root_resource) {
        unittest_printf_critical(" Root resource not available, skipping");
        return true;
    }

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    // Create a vmo and a clone of the same size.
    zx::iommu iommu;
    zx::bti bti;
    zx::vmo vmo;
    if (contiguous) {
        zx_iommu_desc_dummy_t desc;
        ASSERT_EQ(zx_iommu_create(get_root_resource(), ZX_IOMMU_TYPE_DUMMY,
                                  &desc, sizeof(desc), iommu.reset_and_get_address()), ZX_OK);
        ASSERT_EQ(zx::bti::create(iommu, 0, 0xdeadbeef, &bti), ZX_OK);
        ASSERT_EQ(zx::vmo::create_contiguous(bti, 4 * ZX_PAGE_SIZE, 0, &vmo), ZX_OK);
    } else {
        ASSERT_EQ(zx::vmo::create(4 * ZX_PAGE_SIZE, ZX_VMO_RESIZABLE, &vmo), ZX_OK);
    }

    for (unsigned i = 0; i < 4; i++) {
        ASSERT_TRUE(vmo_write(vmo, i + 1, i * ZX_PAGE_SIZE));
    }

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                               0, 4 * ZX_PAGE_SIZE, &clone), ZX_OK);

    // Write to one page in each vmo.
    ASSERT_TRUE(vmo_write(vmo, 5, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_write(clone, 5, 2 * ZX_PAGE_SIZE));

    ASSERT_EQ(vmo_committed_bytes(vmo), 4 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), 2 * ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + 6 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    const zx::vmo& resize_target = resize_child ? clone : vmo;
    const zx::vmo& original_size_vmo = resize_child ? vmo : clone;

    if (contiguous && !resize_child) {
        // Contigous vmos can't be resizable.
        ASSERT_EQ(resize_target.set_size(ZX_PAGE_SIZE), ZX_ERR_UNAVAILABLE);
        END_TEST;
    } else {
        ASSERT_EQ(resize_target.set_size(ZX_PAGE_SIZE), ZX_OK);
    }

    // Check that the data in both vmos is correct.
    for (unsigned i = 0; i < 4; i++) {
        // The index of original_size_vmo's page we wrote to depends on which vmo it is
        uint32_t written_page_idx = resize_child ? 1 : 2;
        // If we're checking the page we wrote to, look for 5, otherwise look for the tagged value.
        uint32_t expected_val = i == written_page_idx ? 5 : i + 1;
        ASSERT_TRUE(vmo_check(original_size_vmo, expected_val, i * ZX_PAGE_SIZE));
    }
    ASSERT_TRUE(vmo_check(resize_target, 1));

    // Check that pages are properly allocated/blamed.
    ASSERT_EQ(vmo_committed_bytes(vmo), (resize_child ? 4 : 1) * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), (resize_child ? 0 : 3) * ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    // Check that growing the shrunk vmo doesn't expose anything.
    ASSERT_EQ(resize_target.set_size(2 * ZX_PAGE_SIZE), ZX_OK);
    ASSERT_TRUE(vmo_check(resize_target, 0, ZX_PAGE_SIZE));

    // Check that writes into the non-resized vmo don't require allocating pages.
    ASSERT_TRUE(vmo_write(original_size_vmo, 6, 3 * ZX_PAGE_SIZE));
    ASSERT_EQ(vmo_committed_bytes(vmo), (resize_child ? 4 : 1) * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), (resize_child ? 0 : 3) * ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    // Check that closing the non-resized vmo frees the the inaccessible pages.
    if (contiguous) {
        ASSERT_TRUE(check_contig_state<4>(bti, vmo));
    }

    // Check that closing the non-resized VMO frees the the inaccessible pages.
    if (resize_child) {
        vmo.reset();
    } else {
        clone.reset();
    }

    ASSERT_TRUE(vmo_check(resize_target, 1));
    ASSERT_EQ(vmo_committed_bytes(resize_target), ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

bool resize_child_test() {
    return resize_test(Contiguity::NonContig, ResizeTarget::Child);
}

bool resize_original_test() {
    return resize_test(Contiguity::NonContig, ResizeTarget::Parent);
}

// Tests that growing a clone exposes zeros and doesn't consume memory on parent writes.
bool resize_grow_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(2, &vmo));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                               0, ZX_PAGE_SIZE, &clone), ZX_OK);

    ASSERT_TRUE(vmo_check(clone, 1));

    ASSERT_EQ(clone.set_size(2 * ZX_PAGE_SIZE), ZX_OK);

    // Check that the new page in the clone is 0.
    ASSERT_TRUE(vmo_check(clone, 0, ZX_PAGE_SIZE));

    // Check that writing to the second page of the original vmo doesn't require
    // forking a page and doesn't affect the clone.
    ASSERT_TRUE(vmo_write(vmo, 3, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone, 0, ZX_PAGE_SIZE));

    ASSERT_EQ(vmo_committed_bytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), 0);
    if (get_root_resource) {
        ASSERT_EQ(original + 2 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

// Tests that a vmo with a child that has a non-zero offset can be truncated without
// affecting the child.
bool resize_offset_child_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(3, &vmo));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone),
              ZX_OK);

    ASSERT_EQ(vmo.set_size(0), ZX_OK);

    ASSERT_TRUE(vmo_check(clone, 2));
    ASSERT_EQ(vmo_committed_bytes(vmo), 0);
    ASSERT_EQ(vmo_committed_bytes(clone), ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

// Tests that resize works with multiple disjoint children.
bool resize_disjoint_child_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    auto test_fn = [original](uint32_t perm[]) -> bool {
        zx::vmo vmo;
        ASSERT_TRUE(init_page_tagged_vmo(3, &vmo));

        // Clone one clone for each page.
        zx::vmo clones[3];
        for (unsigned i = 0; i < 3; i++) {
            ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                                       i * ZX_PAGE_SIZE, ZX_PAGE_SIZE, clones + i), ZX_OK);
            ASSERT_TRUE(vmo_check(clones[i], i + 1));
            ASSERT_EQ(vmo_committed_bytes(clones[i]), 0);
        }

        // Nothing new should have been allocated and everything still belongs to the first vmo.
        ASSERT_EQ(vmo_committed_bytes(vmo), 3 * ZX_PAGE_SIZE);
        if (get_root_resource) {
            ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
        }

        // Shrink two of the clones and then the original, and then check that the
        // remaining clone is okay.
        ASSERT_EQ(clones[perm[0]].set_size(0), ZX_OK);
        ASSERT_EQ(clones[perm[1]].set_size(0), ZX_OK);
        ASSERT_EQ(vmo.set_size(0), ZX_OK);

        ASSERT_TRUE(vmo_check(clones[perm[2]], perm[2] + 1));
        ASSERT_EQ(vmo_committed_bytes(vmo), 0);
        ASSERT_EQ(vmo_committed_bytes(clones[perm[0]]), 0);
        ASSERT_EQ(vmo_committed_bytes(clones[perm[1]]), 0);
        ASSERT_EQ(vmo_committed_bytes(clones[perm[2]]), ZX_PAGE_SIZE);
        if (get_root_resource) {
            ASSERT_EQ(original + 1 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
        }

        ASSERT_EQ(clones[perm[2]].set_size(0), ZX_OK);

        ASSERT_EQ(vmo_committed_bytes(clones[perm[2]]), 0);
        if (get_root_resource) {
            ASSERT_EQ(original, kmem_vmo_mem_usage());
        }

        return true;
    };

    ASSERT_TRUE(call_permutations(test_fn, 3));

    END_TEST;
}

// Tests that resize works when with progressive writes.
bool resize_multiple_progressive_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmo;
    ASSERT_TRUE(init_page_tagged_vmo(3, &vmo));

    // Clone the vmo and fork a page into both.
    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                               0, 2 * ZX_PAGE_SIZE, &clone), ZX_OK);
    ASSERT_TRUE(vmo_write(vmo, 4, 0 * ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_write(clone, 5, 1 * ZX_PAGE_SIZE));

    // Create another clone of the original vmo.
    zx::vmo clone2;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone2), ZX_OK);

    // Resize the first clone, check the contents and allocations.
    ASSERT_EQ(clone.set_size(0), ZX_OK);

    ASSERT_TRUE(vmo_check(vmo, 4, 0 * ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(vmo, 2, 1 * ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(vmo, 3, 2 * ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_check(clone2, 4, 0 * ZX_PAGE_SIZE));

    // Nothing new should have been allocated and everything still belongs to the first vmo.
    ASSERT_EQ(vmo_committed_bytes(vmo), 3 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone), 0 * ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(clone2), 0 * ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    // Resize the original vmo and make sure it frees the necessary pages. Which of the clones
    // gets blamed is implementation dependent.
    ASSERT_EQ(vmo.set_size(0), ZX_OK);
    ASSERT_TRUE(vmo_check(clone2, 4, 0 * ZX_PAGE_SIZE));

    constexpr uint64_t kImplClone1Cost = 0;
    constexpr uint64_t kImplClone2Cost = ZX_PAGE_SIZE;
    static_assert(kImplClone1Cost + kImplClone2Cost == ZX_PAGE_SIZE);
    ASSERT_EQ(vmo_committed_bytes(vmo), 0);
    ASSERT_EQ(vmo_committed_bytes(clone), kImplClone1Cost);
    ASSERT_EQ(vmo_committed_bytes(clone2), kImplClone2Cost);
    if (get_root_resource) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

// Tests the basic operation of the ZX_VMO_ZERO_CHILDREN signal.
bool children_test() {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    zx_signals_t o;
    ASSERT_EQ(vmo.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o), ZX_OK);

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone), ZX_OK);

    ASSERT_EQ(vmo.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o),
              ZX_ERR_TIMED_OUT);
    ASSERT_EQ(clone.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o), ZX_OK);

    clone.reset();

    ASSERT_EQ(vmo.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o), ZX_OK);

    END_TEST;
}

// Tests that child count and zero child signals for when there are many children. Tests
// with closing the children both in the order they were created and the reverse order.
bool many_children_test_body(bool reverse_close) {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    static constexpr uint32_t kCloneCount = 5;
    zx::vmo clones[kCloneCount];

    for (unsigned i = 0; i < kCloneCount; i++) {
        ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, clones + i),
                  ZX_OK);
        ASSERT_EQ(vmo_num_children(vmo), i + 1);
    }

    if (reverse_close) {
        for (unsigned i = kCloneCount - 1; i != UINT32_MAX; i--) {
            clones[i].reset();
            ASSERT_EQ(vmo_num_children(vmo), i);
        }
    } else {
        for (unsigned i = 0; i < kCloneCount; i++) {
            clones[i].reset();
            ASSERT_EQ(vmo_num_children(vmo), kCloneCount - (i + 1));
        }
    }

    zx_signals_t o;
    ASSERT_EQ(vmo.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o), ZX_OK);

    END_TEST;
}

bool many_children_test() {
    return many_children_test_body(false);
}

bool many_children_rev_close_test() {
    return many_children_test_body(true);
}

// Creates a collection of clones and writes to their mappings in every permutation order
// to make sure that no order results in a bad read.
bool many_clone_mapping_test() {
    BEGIN_TEST;

    constexpr uint32_t kNumElts = 4;

    auto test_fn = [](uint32_t perm[]) -> bool {
        zx::vmo vmos[kNumElts];
        ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, vmos), ZX_OK);

        constexpr uint32_t kOriginalData = 0xdeadbeef;
        constexpr uint32_t kNewData = 0xc0ffee;

        ASSERT_TRUE(vmo_write(vmos[0], kOriginalData));

        ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                       0, ZX_PAGE_SIZE, vmos + 1), ZX_OK);
        ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                       0, ZX_PAGE_SIZE, vmos + 2), ZX_OK);
        ASSERT_EQ(vmos[1].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                       0, ZX_PAGE_SIZE, vmos + 3), ZX_OK);

        Mapping mappings[kNumElts] = {};

        // Map the vmos and make sure they're all correct.
        for (unsigned i = 0; i < kNumElts; i++) {
            ASSERT_EQ(mappings[i].Init(vmos[i], ZX_PAGE_SIZE), ZX_OK);
            ASSERT_EQ(*mappings[i].ptr(), kOriginalData);
        }

        // Write to the pages in the order specified by |perm| and validate.
        bool written[kNumElts] = {};
        for (unsigned i = 0; i < kNumElts; i++) {
            uint32_t cur_idx = perm[i];
            *mappings[cur_idx].ptr() = kNewData;
            written[cur_idx] = true;

            for (unsigned j = 0; j < kNumElts; j++) {
                if (*mappings[j].ptr() != (written[j] ? kNewData : kOriginalData)) {
                    return false;
                }
            }
        }

        return true;
    };

    ASSERT_TRUE(call_permutations(test_fn, kNumElts));

    END_TEST;
}

// Tests that a chain of clones where some have offsets works.
bool many_clone_offset_test() {
    BEGIN_TEST;

    zx::vmo vmo;
    zx::vmo clone1;
    zx::vmo clone2;

    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    ASSERT_TRUE(vmo_write(vmo, 1));

    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone1), ZX_OK);
    ASSERT_EQ(clone1.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone2),
              ZX_OK);

    vmo_write(clone1, 1);

    clone1.reset();

    ASSERT_TRUE(vmo_check(vmo, 1));

    END_TEST;
}

// Tests that a chain of clones where some have offsets doesn't mess up
// the page migration logic.
bool many_clone_mapping_offset_test() {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    zx::vmo vmos[4];
    ASSERT_EQ(zx::vmo::create(2 * ZX_PAGE_SIZE, 0, vmos), ZX_OK);


    ASSERT_TRUE(vmo_write(vmos[0], 1));

    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   0, 2 * ZX_PAGE_SIZE, vmos + 1), ZX_OK);
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   ZX_PAGE_SIZE, ZX_PAGE_SIZE, vmos + 2), ZX_OK);
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   0, 2 * ZX_PAGE_SIZE, vmos + 3), ZX_OK);

    Mapping mappings[4] = {};

    // Map the vmos and make sure they're all correct.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_EQ(mappings[i].Init(vmos[i], ZX_PAGE_SIZE), ZX_OK);
        if (i != 2) {
            ASSERT_EQ(*mappings[i].ptr(), 1);
        }
    }

    ASSERT_TRUE(vmo_write(vmos[3], 2));
    ASSERT_TRUE(vmo_write(vmos[1], 3));

    ASSERT_EQ(*mappings[1].ptr(), 3);
    ASSERT_EQ(*mappings[3].ptr(), 2);
    ASSERT_EQ(*mappings[0].ptr(), 1);

    if (original) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_EQ(vmo_committed_bytes(vmos[i]), (i != 2) * ZX_PAGE_SIZE);
    }

    END_TEST;
}

// Tests the correctness and memory consumption of a chain of progressive clones, and
// ensures that memory is properly discarded by closing/resizing the vmos.
uint64_t progressive_clone_discard_test(bool close) {
    BEGIN_TEST;

    uint64_t original = 0;
    if (get_root_resource) {
        original = kmem_vmo_mem_usage();
    }

    constexpr uint64_t kNumClones = 6;
    zx::vmo vmos[kNumClones];
    ASSERT_TRUE(init_page_tagged_vmo(kNumClones, vmos));

    ASSERT_EQ(vmo_committed_bytes(vmos[0]), kNumClones * ZX_PAGE_SIZE);
    if (get_root_resource) {
        ASSERT_EQ(original + kNumClones * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    // Repeatedly clone the vmo while simultaniously changing it. Then check the total memory
    // consumption. This must consume less pages than manually duplicating the vmo, but the
    // precise amount consumed and the amount blamed to each vmo is implementation dependent.
    // Furthermore, the amount blamed should match the amount allocated.
    for (unsigned i = 1; i < kNumClones; i++) {
        ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                                       0, kNumClones * ZX_PAGE_SIZE, vmos + i), ZX_OK);
        ASSERT_TRUE(vmo_write(vmos[i], kNumClones + 2, i * ZX_PAGE_SIZE));
    }
    constexpr uint64_t kImplTotalPages = (kNumClones * (kNumClones + 1)) / 2;
    static_assert(kImplTotalPages <= kNumClones * kNumClones);
    for (unsigned i = 0; i < kNumClones; i++) {
        ASSERT_EQ(vmo_committed_bytes(vmos[i]), (kNumClones - i) * ZX_PAGE_SIZE);
    }
    if (get_root_resource) {
        ASSERT_EQ(original + kImplTotalPages * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    // Check that the vmos have the right content.
    for (unsigned i = 0; i < kNumClones; i++) {
        for (unsigned j = 0; j < kNumClones; j++) {
            uint32_t expected = (i != 0 && j == i) ? kNumClones + 2 : j + 1;
            ASSERT_TRUE(vmo_check(vmos[i], expected, j * ZX_PAGE_SIZE));
        }
    }

    // Close the original vmo and check for correctness.
    if (close) {
        vmos[0].reset();
    } else {
        ASSERT_EQ(vmos[0].set_size(0), ZX_OK);
    }

    for (unsigned i = 1; i < kNumClones; i++) {
        for (unsigned j = 0; j < kNumClones; j++) {
            ASSERT_TRUE(vmo_check(vmos[i], j == i ? kNumClones + 2 : j + 1, j * ZX_PAGE_SIZE));
        }
    }

    // Check that some memory was freed and that all allocated memory is accounted for. The total
    // amount retained is implementation dependent, but it must be less than manually copying
    // the vmo. The amount blamed to each vmo does not need to be the same for both version
    // of this test.
    constexpr uint64_t kImplRemainingPages = kImplTotalPages - 1;
    static_assert(kImplRemainingPages <= kNumClones * (kNumClones - 1));
    uint64_t observed = 0;
    for (unsigned i = 1; i < kNumClones; i++) {
        observed += vmo_committed_bytes(vmos[i]);
    }
    if (get_root_resource) {
        ASSERT_EQ(original + observed, kmem_vmo_mem_usage());
    }
    ASSERT_EQ(observed, kImplRemainingPages * ZX_PAGE_SIZE);

    // Close all but the last two vmos. The total amount of memory consumed by the two remaining
    // vmos is *not* implementation dependent.
    for (unsigned i = 1; i < kNumClones - 2; i++) {
        if (close) {
            vmos[i].reset();
        } else {
            ASSERT_EQ(vmos[i].set_size(0), ZX_OK);
        }
    }

    for (unsigned i = kNumClones - 2; i < kNumClones; i++) {
        for (unsigned j = 0; j < kNumClones; j++) {
            ASSERT_TRUE(vmo_check(vmos[i], j == i ? kNumClones + 2 : j + 1, j * ZX_PAGE_SIZE));
        }
    }
    if (get_root_resource) {
        ASSERT_EQ(original + (kNumClones + 2) * ZX_PAGE_SIZE, kmem_vmo_mem_usage());
    }

    END_TEST;
}

bool progressive_clone_close_test() {
    return progressive_clone_discard_test(true);
}

bool progressive_clone_truncate_test() {
    return progressive_clone_discard_test(false);
}

// Tests that a contiguous VMO remains contiguous even after writes to its clones.
bool contiguous_vmo_test() {
    BEGIN_TEST;

    if (!get_root_resource) {
        unittest_printf_critical(" Root resource not available, skipping");
        return true;
    }

    zx::iommu iommu;
    zx::bti bti;
    zx_iommu_desc_dummy_t desc;
    ASSERT_EQ(zx_iommu_create(get_root_resource(), ZX_IOMMU_TYPE_DUMMY,
                              &desc, sizeof(desc), iommu.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(zx::bti::create(iommu, 0, 0xdeadbeef, &bti), ZX_OK);

    zx::vmo vmos[4];
    ASSERT_EQ(zx::vmo::create_contiguous(bti, 4 * ZX_PAGE_SIZE, 0, vmos), ZX_OK);

    // Tag each page.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_TRUE(vmo_write(vmos[0], i + 1, i * ZX_PAGE_SIZE));
    }

    // Create two clones of the original VMO and one clone of one of those clones.
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 4 * ZX_PAGE_SIZE, vmos + 1),
              ZX_OK);
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 4 * ZX_PAGE_SIZE, vmos + 2),
              ZX_OK);
    ASSERT_EQ(vmos[1].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 4 * ZX_PAGE_SIZE, vmos + 3),
              ZX_OK);

    // Write to one page in each different VMO.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_TRUE(vmo_write(vmos[i], 5, i * ZX_PAGE_SIZE));
    }

    // Verify that the data is correct in each VMO.
    for (unsigned i = 0; i < 4; i++) {
        for (unsigned j = 0; j < 4; j++) {
            ASSERT_TRUE(vmo_check(vmos[i], i == j ? 5 : j + 1, j * ZX_PAGE_SIZE));
        }
    }

    ASSERT_TRUE(check_contig_state<4>(bti, vmos[0]));

    END_TEST;
}

// Tests that closing the clone of a contiguous VMO doesn't cause problems with contiguity.
bool contiguous_vmo_close_child_test() {
    BEGIN_TEST;

    if (!get_root_resource) {
        unittest_printf_critical(" Root resource not available, skipping");
        return true;
    }

    zx::iommu iommu;
    zx::bti bti;
    zx_iommu_desc_dummy_t desc;
    ASSERT_EQ(zx_iommu_create(get_root_resource(), ZX_IOMMU_TYPE_DUMMY,
                              &desc, sizeof(desc), iommu.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(zx::bti::create(iommu, 0, 0xdeadbeef, &bti), ZX_OK);

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create_contiguous(bti, 2 * ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    ASSERT_TRUE(vmo_write(vmo, 1, 0), ZX_OK);
    ASSERT_TRUE(vmo_write(vmo, 2, ZX_PAGE_SIZE), ZX_OK);

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone), ZX_OK);

    // Write to one page in the contig VMO so that one page is forked and one page isn't forked.
    ASSERT_TRUE(vmo_write(vmo, 3, 0));

    // Reset the original VMO and check that things got properly merged into the child.
    clone.reset();

    ASSERT_TRUE(vmo_check(vmo, 3, 0));
    ASSERT_TRUE(vmo_check(vmo, 2, ZX_PAGE_SIZE));
    ASSERT_TRUE(check_contig_state<2>(bti, vmo));

    END_TEST;
}

// Tests that pages are properly become 'non-contiguous' after closing a contiguous VMO
// with a child.
bool contiguous_vmo_close_original_test() {
    BEGIN_TEST;

    if (!get_root_resource) {
        unittest_printf_critical(" Root resource not available, skipping");
        return true;
    }

    uint64_t original = kmem_vmo_mem_usage();

    zx::iommu iommu;
    zx::bti bti;
    zx_iommu_desc_dummy_t desc;
    ASSERT_EQ(zx_iommu_create(get_root_resource(), ZX_IOMMU_TYPE_DUMMY,
                              &desc, sizeof(desc), iommu.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(zx::bti::create(iommu, 0, 0xdeadbeef, &bti), ZX_OK);

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create_contiguous(bti, 3 * ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    ASSERT_TRUE(vmo_write(vmo, 1, 0), ZX_OK);
    ASSERT_TRUE(vmo_write(vmo, 2, ZX_PAGE_SIZE), ZX_OK);
    ASSERT_TRUE(vmo_write(vmo, 3, 2 * ZX_PAGE_SIZE), ZX_OK);

    // Create the clone so that there is a page before and after it.
    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone),
              ZX_OK);

    ASSERT_TRUE(vmo_check(clone, 2));

    vmo.reset();

    ASSERT_TRUE(vmo_check(clone, 2));
    ASSERT_EQ(vmo_committed_bytes(clone), ZX_PAGE_SIZE);
    ASSERT_EQ(original + ZX_PAGE_SIZE, kmem_vmo_mem_usage());

    END_TEST;
}

bool contiguous_vmo_resize_child_test() {
    return resize_test(Contiguity::Contig, ResizeTarget::Child);
}

bool contiguous_vmo_resize_original_test() {
    return resize_test(Contiguity::Contig, ResizeTarget::Parent);
}

// Tests partial clones of contiguous vmos.
bool contiguous_vmo_partial_clone_test() {
    BEGIN_TEST;

    if (!get_root_resource) {
        unittest_printf_critical(" Root resource not available, skipping");
        return true;
    }

    zx::iommu iommu;
    zx::bti bti;
    zx_iommu_desc_dummy_t desc;
    ASSERT_EQ(zx_iommu_create(get_root_resource(), ZX_IOMMU_TYPE_DUMMY,
                              &desc, sizeof(desc), iommu.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(zx::bti::create(iommu, 0, 0xdeadbeef, &bti), ZX_OK);

    zx::vmo vmos[4];
    ASSERT_EQ(zx::vmo::create_contiguous(bti, 3 * ZX_PAGE_SIZE, 0, vmos), ZX_OK);

    // Tag each page.
    for (unsigned i = 0; i < 3; i++) {
        ASSERT_TRUE(vmo_write(vmos[0], i + 1, i * ZX_PAGE_SIZE));
    }

    // Create two clones of the original VMO and one clone of one of those clones.
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 1), ZX_OK);
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 2), ZX_OK);
    ASSERT_EQ(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 4 * ZX_PAGE_SIZE, vmos + 3),
              ZX_OK);

    ASSERT_TRUE(vmo_write(vmos[0], 5, ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_write(vmos[3], 6, ZX_PAGE_SIZE));

    ASSERT_TRUE(vmo_write(vmos[3], 6, 2 * ZX_PAGE_SIZE));
    ASSERT_TRUE(vmo_write(vmos[0], 5, 2 * ZX_PAGE_SIZE));

    // Verify that the data is correct in each VMO.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_TRUE(vmo_check(vmos[i], 1, 0 * ZX_PAGE_SIZE));
        if (i == 0 || i == 3) {
            uint32_t target_val = i == 0 ? 5 : 6;
            ASSERT_TRUE(vmo_check(vmos[i], target_val, 1 * ZX_PAGE_SIZE));
            ASSERT_TRUE(vmo_check(vmos[i], target_val, 2 * ZX_PAGE_SIZE));
        }
    }

    ASSERT_TRUE(check_contig_state<3>(bti, vmos[0]));

    END_TEST;
}

// Tests that clones based on physical vmos can't be created.
bool no_physical_test() {
    BEGIN_TEST;

    if (!get_root_resource) {
        unittest_printf_critical(" Root resource not available, skipping");
        return true;
    }

    zx::vmo vmo;
    ASSERT_EQ(zx_vmo_create_physical(
                get_root_resource(), 0, ZX_PAGE_SIZE, vmo.reset_and_get_address()), ZX_OK);

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone),
              ZX_ERR_NOT_SUPPORTED);

    END_TEST;
}

// Tests that clones based on pager vmos can't be created.
bool no_pager_test() {
    BEGIN_TEST;

    zx::pager pager;
    ASSERT_EQ(zx::pager::create(0, &pager), ZX_OK);

    zx::port port;
    ASSERT_EQ(zx::port::create(0, &port), ZX_OK);

    zx::vmo vmo;
    ASSERT_EQ(pager.create_vmo(ZX_VMO_NON_RESIZABLE, port, 0, ZX_PAGE_SIZE, &vmo), ZX_OK);

    zx::vmo uni_clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE, 0, ZX_PAGE_SIZE, &uni_clone), ZX_OK);

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone),
                               ZX_ERR_NOT_SUPPORTED);
    ASSERT_EQ(uni_clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                     0, ZX_PAGE_SIZE, &clone), ZX_ERR_NOT_SUPPORTED);

    END_TEST;
}

// Tests that clones of uncached memory can't be created.
bool uncached_test() {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    ASSERT_EQ(vmo.set_cache_policy(ZX_CACHE_POLICY_UNCACHED), ZX_OK);

    Mapping vmo_mapping;
    ASSERT_EQ(vmo_mapping.Init(vmo, ZX_PAGE_SIZE), ZX_OK);

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    *vmo_mapping.ptr() = kOriginalData;

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone),
              ZX_ERR_BAD_STATE);

    ASSERT_EQ(*vmo_mapping.ptr(), kOriginalData);

    END_TEST;
}

} // namespace

BEGIN_TEST_CASE(vmo_clone2_tests)
RUN_TEST(info_test)
RUN_TEST(read_test)
RUN_TEST(clone_vmo_write_test)
RUN_TEST(parent_vmo_write_test)
RUN_TEST(clone_vmar_write_test)
RUN_TEST(parent_vmar_write_test)
RUN_TEST(close_clone_test)
RUN_TEST(close_original_test)
RUN_TEST(obj_mem_accounting_test)
RUN_TEST(kmem_accounting_test)
RUN_TEST(zero_page_write_test)
RUN_TEST(offset_test)
RUN_TEST(overflow_test)
RUN_TEST(small_clone_test)
RUN_TEST(small_clone_child_test)
RUN_TEST(small_clones_test)
RUN_TEST(disjoint_clone_early_close_test)
RUN_TEST(disjoint_clone_late_close_test)
RUN_TEST(disjoint_clone_test2)
RUN_TEST(resize_child_test)
RUN_TEST(resize_original_test)
RUN_TEST(resize_grow_test)
RUN_TEST(resize_offset_child_test)
RUN_TEST(resize_disjoint_child_test)
RUN_TEST(resize_multiple_progressive_test)
RUN_TEST(children_test)
RUN_TEST(many_children_test)
RUN_TEST(many_children_rev_close_test)
RUN_TEST(many_clone_mapping_test)
RUN_TEST(many_clone_offset_test)
RUN_TEST(many_clone_mapping_offset_test)
RUN_TEST(progressive_clone_close_test)
RUN_TEST(progressive_clone_truncate_test)
RUN_TEST(contiguous_vmo_test)
RUN_TEST(contiguous_vmo_close_child_test)
RUN_TEST(contiguous_vmo_close_original_test)
RUN_TEST(contiguous_vmo_resize_child_test)
RUN_TEST(contiguous_vmo_resize_original_test)
RUN_TEST(contiguous_vmo_partial_clone_test)
RUN_TEST(no_physical_test)
RUN_TEST(no_pager_test)
RUN_TEST(uncached_test)
END_TEST_CASE(vmo_clone2_tests)
