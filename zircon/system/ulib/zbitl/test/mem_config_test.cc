// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "../mem_config.h"

#include <lib/zbitl/error_string.h>
#include <lib/zbitl/image.h>
#include <lib/zbitl/items/mem_config.h>
#include <lib/zbitl/memory.h>
#include <lib/zbitl/view.h>
#include <zircon/boot/e820.h>
#include <zircon/boot/image.h>

#include <efi/boot-services.h>
#include <efi/runtime-services.h>
#include <fbl/array.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace {

// GMock matcher to determine if a given zbitl::View result was successful,
// printing the error if not.
//
// Can be used as: EXPECT_THAT(view.operation(), IsOk());
MATCHER(IsOk, "") {
  if (arg.is_ok()) {
    return true;
  }
  *result_listener << "had error: " << zbitl::ViewErrorString(arg.error_value());
  return false;
}

using ZbiMemoryImage = zbitl::Image<fbl::Array<std::byte>>;

// Create an empty zbitl::Image that can be written to.
ZbiMemoryImage CreateImage() {
  ZbiMemoryImage image;

  // Initialise the ZBI header
  auto result = image.clear();
  ZX_ASSERT(result.is_ok());

  return image;
}

// Return a zbitl::View of the given ZbiMemoryImage.
zbitl::View<zbitl::ByteView> AsView(const ZbiMemoryImage& image) {
  return zbitl::View(zbitl::ByteView(image.storage().data(), image.storage().size()));
}

// Append the given payload to a zbitl::Image.
//
// Abort on error.
void AppendPayload(ZbiMemoryImage& zbi, uint32_t type, zbitl::ByteView bytes) {
  auto result = zbi.Append(zbi_header_t{.type = type}, bytes);
  ZX_ASSERT(result.is_ok());
}

// Append the given objects together as a series of bytes.
template <typename... T>
std::basic_string<std::byte> JoinBytes(const T&... object) {
  std::basic_string<std::byte> result;

  // Add the bytes from a single item to |result|.
  auto add_item = [&result](const auto& x) {
    zbitl::ByteView object_bytes = zbitl::AsBytes(x);
    result.append(object_bytes);
  };

  // Add each item.
  (add_item(object), ...);

  return result;
}

// Determine if two `zbi_mem_range_t` values are the same.
bool MemRangeEqual(const zbi_mem_range_t& a, const zbi_mem_range_t& b) {
  return std::tie(a.length, a.paddr, a.reserved, a.type) ==
         std::tie(b.length, b.paddr, b.reserved, b.type);
}

TEST(ToMemRange, Efi) {
  constexpr efi_memory_descriptor efi{
      .Type = EfiConventionalMemory,
      .PhysicalStart = 0x1234'abcd'ffff'0000,
      .VirtualStart = 0xaaaa'aaaa'aaaa'aaaa,
      .NumberOfPages = 100,
      .Attribute = EFI_MEMORY_MORE_RELIABLE,
  };
  constexpr zbi_mem_range_t expected{
      .paddr = 0x1234'abcd'ffff'0000,
      .length = 409600,  // ZX_PAGE_SIZE * 100
      .type = ZBI_MEM_RANGE_RAM,
  };
  EXPECT_TRUE(MemRangeEqual(zbitl::internal::ToMemRange(efi), expected));
}

TEST(ToMemRange, EfiReservedMemory) {
  auto efi = efi_memory_descriptor{
      .Type = EfiMemoryMappedIO,
      .PhysicalStart = 0x0,
      .VirtualStart = 0x0,
      .NumberOfPages = 1,
      .Attribute = 0,
  };
  EXPECT_EQ(zbitl::internal::ToMemRange(efi).type, static_cast<uint32_t>(ZBI_MEM_RANGE_RESERVED));
}

TEST(ToMemRange, E820) {
  auto input = e820entry_t{
      .addr = 0x1234'abcd'ffff'0000,
      .size = 0x10'0000,
      .type = E820_RAM,
  };
  auto expected = zbi_mem_range_t{
      .paddr = 0x1234'abcd'ffff'0000,
      .length = 0x10'0000,
      .type = ZBI_MEM_RANGE_RAM,
  };
  EXPECT_TRUE(MemRangeEqual(zbitl::internal::ToMemRange(input), expected));
}

TEST(MemRangeIterator, DefaultContainer) {
  zbitl::MemRangeTable container;

  EXPECT_EQ(container.begin(), container.end());
  EXPECT_THAT(container.take_error(), IsOk());
}

TEST(MemRangeIterator, EmptyZbi) {
  ZbiMemoryImage zbi = CreateImage();
  zbitl::MemRangeTable container{AsView(zbi)};

  // Expect nothing to be found.
  EXPECT_EQ(container.begin(), container.end());
  EXPECT_THAT(container.take_error(), IsOk());
}

TEST(MemRangeIterator, BadZbi) {
  zbi_header_t header = ZBI_CONTAINER_HEADER(0);
  header.crc32 = 0xffffffff;  // bad CRC.
  zbitl::View<zbitl::ByteView> view(zbitl::AsBytes(header));
  zbitl::MemRangeTable container{view};

  // Expect nothing to be found.
  EXPECT_EQ(container.begin(), container.end());

  // Expect an error.
  auto error = container.take_error();
  ASSERT_TRUE(error.is_error());
  EXPECT_EQ(error.error_value().zbi_error, "bad crc32 field in item without CRC");
}

TEST(MemRangeIterator, RequireErrorToBeCalled) {
  ZbiMemoryImage zbi = CreateImage();

  // Iterate through an empty item and then destroy it without calling Error().
  ASSERT_DEATH(
      {
        zbitl::MemRangeTable container{AsView(zbi)};

        // Expect nothing to be found.
        EXPECT_EQ(container.begin(), container.end());

        // Don't call `take_error`: expect process death during object destruction.
      },
      "destroyed .* without check");
}

TEST(MemRangeIterator, NoErrorNeededAfterMove) {
  ZbiMemoryImage zbi = CreateImage();
  zbitl::MemRangeTable container{AsView(zbi)};

  // Iterate through an empty item.
  container.begin();

  // Move the value, and check the error in its new location. We shouldn't
  // need to check the first any longer.
  zbitl::MemRangeTable new_container = std::move(container);
  EXPECT_THAT(new_container.take_error(), IsOk());
}

TEST(MemRangeIterator, EmptyPayload) {
  // Construct a ZBI with an empty E820 memory map.
  ZbiMemoryImage zbi = CreateImage();
  AppendPayload(zbi, ZBI_TYPE_E820_TABLE, {});

  // Expect nothing to be found.
  zbitl::MemRangeTable container{AsView(zbi)};
  EXPECT_EQ(container.begin(), container.end());
  EXPECT_THAT(container.take_error(), IsOk());
}

TEST(MemRangeIterator, EfiItem) {
  // Construct a ZBI with a single payload consisting of EFI entries.
  ZbiMemoryImage zbi = CreateImage();
  AppendPayload(zbi, ZBI_TYPE_EFI_MEMORY_MAP,
                JoinBytes(
                    efi_memory_descriptor{
                        .PhysicalStart = 0x1000,
                        .NumberOfPages = 1,
                    },
                    efi_memory_descriptor{
                        .PhysicalStart = 0x2000,
                        .NumberOfPages = 1,
                    }));

  // Ensure the entries are correct.
  zbitl::MemRangeTable container{AsView(zbi)};
  std::vector<zbi_mem_range_t> ranges(container.begin(), container.end());
  ASSERT_TRUE(container.take_error().is_ok());
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].paddr, 0x1000u);
  EXPECT_EQ(ranges[1].paddr, 0x2000u);
}

TEST(MemRangeIterator, ZbiMemRangeItem) {
  // Construct a ZBI with a single payload consisting of zbi_mem_range_t entries.
  ZbiMemoryImage zbi = CreateImage();
  AppendPayload(zbi, ZBI_TYPE_MEM_CONFIG,
                JoinBytes(
                    zbi_mem_range_t{
                        .paddr = 0x1000,
                        .length = 0x1000,
                    },
                    zbi_mem_range_t{
                        .paddr = 0x2000,
                        .length = 0x1000,
                    }));

  // Ensure the entries are correct.
  zbitl::MemRangeTable container{AsView(zbi)};
  std::vector<zbi_mem_range_t> ranges(container.begin(), container.end());
  ASSERT_TRUE(container.take_error().is_ok());
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].paddr, 0x1000u);
  EXPECT_EQ(ranges[1].paddr, 0x2000u);
}

TEST(MemRangeIterator, E820Item) {
  // Construct a ZBI with a single payload consisting of e820entry_t entries.
  ZbiMemoryImage zbi = CreateImage();
  AppendPayload(zbi, ZBI_TYPE_E820_TABLE,
                JoinBytes(
                    e820entry_t{
                        .addr = 0x1000,
                        .size = 0x1000,
                    },
                    e820entry_t{
                        .addr = 0x2000,
                        .size = 0x1000,
                    }));

  // Ensure the entries are correct.
  zbitl::MemRangeTable container{AsView(zbi)};
  std::vector<zbi_mem_range_t> ranges(container.begin(), container.end());
  ASSERT_TRUE(container.take_error().is_ok());
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].paddr, 0x1000u);
  EXPECT_EQ(ranges[1].paddr, 0x2000u);
}

TEST(MemRangeIterator, MixedItems) {
  // Construct a ZBI with a mixed set of payloads.
  ZbiMemoryImage zbi = CreateImage();
  AppendPayload(zbi, ZBI_TYPE_E820_TABLE,
                zbitl::AsBytes(e820entry_t{
                    .addr = 0x1000,
                    .size = 0x1000,
                }));
  AppendPayload(zbi, ZBI_TYPE_MEM_CONFIG,
                zbitl::AsBytes(zbi_mem_range_t{
                    .paddr = 0x2000,
                    .length = 0x2000,
                }));
  AppendPayload(zbi, ZBI_TYPE_EFI_MEMORY_MAP,
                zbitl::AsBytes(efi_memory_descriptor{
                    .PhysicalStart = 0x3000,
                    .NumberOfPages = 3,
                }));

  // Ensure the entries are correct.
  zbitl::MemRangeTable container{AsView(zbi)};
  std::vector<zbi_mem_range_t> ranges(container.begin(), container.end());
  ASSERT_TRUE(container.take_error().is_ok());
  ASSERT_EQ(ranges.size(), 3u);
  EXPECT_EQ(ranges[0].paddr, 0x1000u);
  EXPECT_EQ(ranges[1].paddr, 0x2000u);
  EXPECT_EQ(ranges[2].paddr, 0x3000u);
}

TEST(MemRangeIterator, OtherItems) {
  // Construct a ZBI with non-memory payloads.
  ZbiMemoryImage zbi = CreateImage();
  AppendPayload(zbi, ZBI_TYPE_PLATFORM_ID, {});
  AppendPayload(zbi, ZBI_TYPE_PLATFORM_ID, {});
  AppendPayload(zbi, ZBI_TYPE_MEM_CONFIG,
                zbitl::AsBytes(zbi_mem_range_t{
                    .paddr = 0x1000,
                    .length = 0x1000,
                }));

  // Ensure the entries are correct.
  zbitl::MemRangeTable container{AsView(zbi)};
  std::vector<zbi_mem_range_t> ranges(container.begin(), container.end());
  ASSERT_TRUE(container.take_error().is_ok());
  ASSERT_EQ(ranges.size(), 1u);
  EXPECT_EQ(ranges[0].paddr, 0x1000u);
}

}  // namespace
