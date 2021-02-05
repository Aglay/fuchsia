// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_STDCOMPAT_INCLUDE_LIB_STDCOMPAT_INTERNAL_BIT_H_
#define LIB_STDCOMPAT_INCLUDE_LIB_STDCOMPAT_INTERNAL_BIT_H_

#include <limits>
#include <type_traits>

namespace cpp20 {
namespace internal {

// Helper for exclusing specific char types when char is considered unsigned by the compiler.
template <typename T>
struct is_unsigned {
  static constexpr const bool value =
      std::is_unsigned<T>::value && !std::is_same<bool, T>::value &&
      !std::is_same<T, char>::value && !std::is_same<T, char16_t>::value &&
      !std::is_same<T, char32_t>::value && !std::is_same<T, wchar_t>::value;
};

// Rotation implementation.
// Only internal for usage in implementaiton of certain methods.
template <class T>
[[gnu::warn_unused_result]] constexpr std::enable_if_t<is_unsigned<T>::value, T> rotl(
    T x, int s) noexcept {
  const auto digits = std::numeric_limits<T>::digits;
  const auto rotate_by = s % digits;

  if (rotate_by > 0) {
    return (x << rotate_by) | (x >> (digits - rotate_by));
  }

  if (rotate_by < 0) {
    return (x >> -rotate_by) | (x << (digits + rotate_by));
  }

  return x;
}

template <class T>
[[gnu::warn_unused_result]] constexpr std::enable_if_t<is_unsigned<T>::value, T> rotr(
    T x, int s) noexcept {
  auto digits = std::numeric_limits<T>::digits;
  auto rotate_by = s % digits;

  if (rotate_by > 0) {
    return (x >> rotate_by) | (x << (digits - rotate_by));
  }

  if (rotate_by < 0) {
    return rotl(x, -rotate_by);
  }

  return x;
}

// Overloads for intrinsics.
// Precondition: |value| != 0.
template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(T) <= sizeof(unsigned), int>
count_zeros_from_right(T value) noexcept {
  return __builtin_ctz(static_cast<unsigned>(value));
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned) < sizeof(T) &&
                               sizeof(T) <= sizeof(unsigned long),
                           int>
count_zeros_from_right(T value) noexcept {
  return __builtin_ctzl(static_cast<unsigned long>(value));
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned long) < sizeof(T) &&
                               sizeof(T) <= sizeof(unsigned long long),
                           int>
count_zeros_from_right(T value) noexcept {
  return __builtin_ctzll(static_cast<unsigned long long>(value));
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned long long) < sizeof(T), int>
count_zeros_from_right(T value) noexcept {
  int count = 0;
  int iter_count = 0;
  const unsigned int max_digits = std::numeric_limits<unsigned long long>::digits;

  for (int slot = 0; slot * max_digits < std::numeric_limits<T>::digits; ++slot) {
    const unsigned long long chunk =
        static_cast<unsigned long long>(internal::rotr(value, (slot)*max_digits));
    iter_count = (chunk == 0) ? static_cast<int>(max_digits) : count_zeros_from_right(chunk);
    count += iter_count;
    if (iter_count != max_digits) {
      break;
    }
  }
  return count - static_cast<int>(std::numeric_limits<T>::digits % max_digits);
}

template <typename T, typename U,
          typename std::enable_if<sizeof(T) >= sizeof(U), bool>::type = true>
struct digit_diff
    : std::integral_constant<T, std::numeric_limits<T>::digits - std::numeric_limits<U>::digits> {};

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(T) <= sizeof(unsigned), int>
count_zeros_from_left(T value) noexcept {
  return __builtin_clz(static_cast<unsigned>(value)) -
         static_cast<int>(digit_diff<unsigned, T>::value);
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned) < sizeof(T) &&
                               sizeof(T) <= sizeof(unsigned long),
                           int>
count_zeros_from_left(T value) noexcept {
  return __builtin_clzl(static_cast<unsigned long>(value)) -
         static_cast<int>(digit_diff<unsigned long, T>::value);
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned long) < sizeof(T) &&
                               sizeof(T) <= sizeof(unsigned long long),
                           int>
count_zeros_from_left(T value) noexcept {
  return __builtin_clzll(static_cast<unsigned long long>(value)) -
         static_cast<int>(digit_diff<unsigned long long, T>::value);
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned long long) < sizeof(T), int>
count_zeros_from_left(T value) noexcept {
  int count = 0;
  int iter_count = 0;
  const unsigned int max_digits = std::numeric_limits<unsigned long long>::digits;

  for (int slot = 0; slot * max_digits < std::numeric_limits<T>::digits; ++slot) {
    const unsigned long long chunk =
        static_cast<unsigned long long>(internal::rotl(value, (slot + 1) * max_digits));
    iter_count = (chunk == 0) ? static_cast<int>(max_digits) : count_zeros_from_left(chunk);
    count += iter_count;
    if (iter_count != max_digits) {
      break;
    }
  }
  return count - static_cast<int>(std::numeric_limits<T>::digits % max_digits);
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(T) <= sizeof(unsigned), int> popcount(
    T value) noexcept {
  return __builtin_popcount(static_cast<unsigned>(value));
}

template <typename T>
inline constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned) < sizeof(T) &&
                                      sizeof(T) <= sizeof(unsigned long),
                                  int>
popcount(T value) noexcept {
  return __builtin_popcountl(static_cast<unsigned long>(value));
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned long) < sizeof(T) &&
                               sizeof(T) <= sizeof(unsigned long long),
                           int>
popcount(T value) noexcept {
  return __builtin_popcountll(static_cast<unsigned long long>(value));
}

template <typename T>
constexpr std::enable_if_t<is_unsigned<T>::value && sizeof(unsigned long long) < sizeof(T), int>
popcount(T value) noexcept {
  int accumulated_count = 0;
  while (value != 0) {
    accumulated_count += popcount(static_cast<unsigned long long>(value));
    value >>= std::numeric_limits<unsigned long long>::digits;
  }
  return accumulated_count;
}

}  // namespace internal
}  // namespace cpp20

#endif  // LIB_STDCOMPAT_INCLUDE_LIB_STDCOMPAT_INTERNAL_BIT_H_
