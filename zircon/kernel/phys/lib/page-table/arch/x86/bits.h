// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_PHYS_LIB_PAGE_TABLE_ARCH_X86_BITS_H_
#define ZIRCON_KERNEL_PHYS_LIB_PAGE_TABLE_ARCH_X86_BITS_H_

#include <lib/page-table/types.h>
#include <limits.h>
#include <zircon/assert.h>
#include <zircon/types.h>

namespace page_table::x86 {

// Generate a mask with the low number of bits set.
//
// For example, Mask(3) = 0b111
constexpr uint64_t Mask(uint64_t num_bits) {
  ZX_DEBUG_ASSERT(num_bits <= sizeof(uint64_t) * CHAR_BIT);
  if (num_bits == sizeof(uint64_t) * CHAR_BIT) {
    return ~uint64_t(0);
  }
  return uint64_t(((uint64_t(1) << num_bits) - 1));
}

// Generate a mask where bits `high` to `low` inclusive are set.
//
// For example, Mask(2, 1) == 0b110
constexpr uint64_t Mask(uint64_t high, uint64_t low) {
  ZX_DEBUG_ASSERT(high >= low);
  return Mask(high - low + 1) << low;
}

// Clear the given range of bits in the given word.
//
// For example, ClearBits(2, 1, 0b1111) == 0b1001
constexpr uint64_t ClearBits(uint64_t high, uint64_t low, uint64_t word) {
  return word & ~(Mask(high - low + 1) << low);
}

// Extract the bits [high:low] from value, returning them in the low bits of
// value.
//
// For example, ExtractBits(4, 2, 0b010100) == 0b101
constexpr uint64_t ExtractBits(uint64_t high, uint64_t low, uint64_t value) {
  uint64_t bit_count = high - low + 1;
  auto pow2 = uint64_t(1) << bit_count;
  return ((value >> low) & (pow2 - 1));
}

// Extract a single bit from the given word.
constexpr uint64_t ExtractBit(uint64_t bit, uint64_t value) { return ExtractBits(bit, bit, value); }

// Set the range of bits [high:low] in `word` to the low bits in `bits`.
constexpr uint64_t SetBits(uint64_t high, uint64_t low, uint64_t word, uint64_t bits) {
  // Clear out any bits already set in the range [high:low].
  word = ClearBits(high, low, word);

  // Bitwise-or the new bits in.
  word |= bits << low;

  return word;
}

// Set the given bit in `word` to the given value.
//
// For example, SetBit(/*index=*/1, /*word=*/0b111, /*bit=*/0) == 0b101
constexpr uint64_t SetBit(uint64_t index, uint64_t word, uint64_t bit) {
  return SetBits(/*high=*/index, /*low=*/index, /*word=*/word, /*bits=*/bit);
}

// Sign extend the low `n` bits.
//
// For example:
//   SignExtend(/*word=*/0x40, /*n=*/8) == 0x0000'0000'0000'0040
//   SignExtend(/*word=*/0x80, /*n=*/8) == 0xffff'ffff'ffff'ff80
constexpr uint64_t SignExtend(uint64_t word, uint64_t n) {
  // Perform an unsigned shift moving the `n`'th bit into bit 63.
  uint64_t shifted = word << (64 - n);

  // Perform a signed shift back to the original position. This will
  // sign-extend the top bits.
  return static_cast<uint64_t>(static_cast<int64_t>(shifted) >> (64 - n));
}

}  // namespace page_table::x86

#endif  // ZIRCON_KERNEL_PHYS_LIB_PAGE_TABLE_ARCH_X86_BITS_H_
