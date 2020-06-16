// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/fidl_codec/printer.h"

#include <lib/syslog/cpp/macros.h>

#include "src/lib/fidl_codec/display_handle.h"

namespace fidl_codec {

const Colors WithoutColors("", "", "", "", "", "");
const Colors WithColors(/*new_reset=*/"\u001b[0m", /*new_red=*/"\u001b[31m",
                        /*new_green=*/"\u001b[32m", /*new_blue=*/"\u001b[34m",
                        /*new_white_on_magenta=*/"\u001b[45m\u001b[37m",
                        /*new_yellow_background=*/"\u001b[103m");

PrettyPrinter::PrettyPrinter(std::ostream& os, const Colors& colors, bool pretty_print,
                             std::string_view line_header, int max_line_size,
                             bool header_on_every_line, int tabulations)
    : os_(os),
      colors_(colors),
      pretty_print_(pretty_print),
      line_header_(line_header),
      max_line_size_(max_line_size),
      header_on_every_line_(header_on_every_line),
      tabulations_(tabulations),
      remaining_size_(max_line_size - tabulations * kTabSize) {
  // Computes the displayed size of the header. The header can contain escape sequences (to add some
  // colors) which don't count as displayed characters. Here we count the number of characters in
  // the line header skiping everything between escape ('\u001b') and 'm'.
  size_t i = 0;
  while (i < line_header.size()) {
    if (line_header[i] == '\u001b') {
      i = line_header.find_first_of('m', i + 1);
      if (i == std::string_view::npos) {
        break;
      }
      ++i;
    } else {
      ++i;
      ++line_header_size_;
    }
  }
}

void PrettyPrinter::DisplayHandle(const zx_handle_info_t& handle) {
  fidl_codec::DisplayHandle(handle, *this);
}

#define BtiPermNameCase(name)      \
  if ((perm & (name)) == (name)) { \
    *this << separator << #name;   \
    separator = " | ";             \
  }

void PrettyPrinter::DisplayBtiPerm(uint32_t perm) {
  if (perm == 0) {
    *this << Red << "0" << ResetColor;
    return;
  }

  *this << Blue;
  const char* separator = "";
  BtiPermNameCase(ZX_BTI_PERM_READ);
  BtiPermNameCase(ZX_BTI_PERM_WRITE);
  BtiPermNameCase(ZX_BTI_PERM_EXECUTE);
  BtiPermNameCase(ZX_BTI_COMPRESS);
  BtiPermNameCase(ZX_BTI_CONTIGUOUS);
  *this << ResetColor;
}

#define CachePolicyNameCase(name)         \
  case name:                              \
    *this << Blue << #name << ResetColor; \
    return

void PrettyPrinter::DisplayCachePolicy(uint32_t cache_policy) {
  switch (cache_policy) {
    CachePolicyNameCase(ZX_CACHE_POLICY_CACHED);
    CachePolicyNameCase(ZX_CACHE_POLICY_UNCACHED);
    CachePolicyNameCase(ZX_CACHE_POLICY_UNCACHED_DEVICE);
    CachePolicyNameCase(ZX_CACHE_POLICY_WRITE_COMBINING);
    default:
      *this << Red << cache_policy << ResetColor;
      return;
  }
}

#define ClockNameCase(name)               \
  case name:                              \
    *this << Blue << #name << ResetColor; \
    return

void PrettyPrinter::DisplayClock(zx_clock_t clock) {
  switch (clock) {
    ClockNameCase(ZX_CLOCK_MONOTONIC);
    ClockNameCase(ZX_CLOCK_UTC);
    ClockNameCase(ZX_CLOCK_THREAD);
    default:
      *this << Red << clock << ResetColor;
      return;
  }
}

void PrettyPrinter::DisplayDuration(zx_duration_t duration_ns) {
  if (duration_ns == ZX_TIME_INFINITE) {
    *this << Blue << "ZX_TIME_INFINITE" << ResetColor;
    return;
  }
  if (duration_ns == ZX_TIME_INFINITE_PAST) {
    *this << Blue << "ZX_TIME_INFINITE_PAST" << ResetColor;
    return;
  }
  *this << Blue;
  if (duration_ns < 0) {
    *this << '-';
    duration_ns = -duration_ns;
  }
  const char* separator = "";
  int64_t nanoseconds = duration_ns % kOneBillion;
  int64_t seconds = duration_ns / kOneBillion;
  if (seconds != 0) {
    int64_t minutes = seconds / kSecondsPerMinute;
    if (minutes != 0) {
      int64_t hours = minutes / kMinutesPerHour;
      if (hours != 0) {
        int64_t days = hours / kHoursPerDay;
        if (days != 0) {
          *this << days << " days";
          separator = ", ";
        }
        *this << separator << (hours % kHoursPerDay) << " hours";
        separator = ", ";
      }
      *this << separator << (minutes % kMinutesPerHour) << " minutes";
      separator = ", ";
    }
    *this << separator << (seconds % kSecondsPerMinute) << " seconds";
    if (nanoseconds != 0) {
      *this << " and " << nanoseconds << " nano seconds";
    }
  } else if (nanoseconds != 0) {
    *this << nanoseconds << " nano seconds";
  } else {
    *this << "0 seconds";
  }
  *this << ResetColor;
}

void PrettyPrinter::DisplayTime(zx_time_t time_ns) {
  if (time_ns == ZX_TIME_INFINITE) {
    (*this) << Blue << "ZX_TIME_INFINITE" << ResetColor;
  } else if (time_ns == ZX_TIME_INFINITE_PAST) {
    (*this) << Blue << "ZX_TIME_INFINITE_PAST" << ResetColor;
  } else {
    // Gets the time in seconds.
    time_t value = time_ns / kOneBillion;
    struct tm tm;
    if (localtime_r(&value, &tm) == &tm) {
      char buffer[100];
      strftime(buffer, sizeof(buffer), "%c", &tm);
      // And now, displays the nano seconds.
      (*this) << Blue << buffer << " and ";
      snprintf(buffer, sizeof(buffer), "%09" PRId64, time_ns % kOneBillion);
      (*this) << buffer << " ns" << ResetColor;
    } else {
      (*this) << Red << "unknown time" << ResetColor;
    }
  }
}

void PrettyPrinter::IncrementTabulations() {
  ++tabulations_;
  if (need_to_print_header_) {
    remaining_size_ -= kTabSize;
  }
}

void PrettyPrinter::DecrementTabulations() {
  --tabulations_;
  if (need_to_print_header_) {
    remaining_size_ += kTabSize;
  }
}

void PrettyPrinter::NeedHeader() {
  remaining_size_ = max_line_size_ - line_header_size_ - tabulations_ * kTabSize;
  need_to_print_header_ = true;
}

void PrettyPrinter::PrintHeader(char first_character) {
  FX_DCHECK(need_to_print_header_);
  need_to_print_header_ = false;
  if (line_header_size_ > 0) {
    os_ << line_header_;
    if (!header_on_every_line_) {
      line_header_size_ = 0;
    }
  }
  if (first_character != '\n') {
    for (int tab = tabulations_ * kTabSize; tab > 0; --tab) {
      os_ << ' ';
    }
  }
}

PrettyPrinter& PrettyPrinter::operator<<(std::string_view data) {
  if (data.empty()) {
    return *this;
  }
  if (need_to_print_header_) {
    PrintHeader(data[0]);
  }
  size_t end_of_line = data.find('\n', 0);
  if (end_of_line == std::string_view::npos) {
    os_ << data;
    remaining_size_ -= data.size();
    return *this;
  }
  size_t current = 0;
  for (;;) {
    std::string_view tmp = data.substr(current, end_of_line - current + 1);
    os_ << tmp;
    NeedHeader();
    current = end_of_line + 1;
    if (current >= data.size()) {
      return *this;
    }
    end_of_line = data.find('\n', current);
    if (end_of_line == std::string_view::npos) {
      os_ << data;
      remaining_size_ -= data.size();
      return *this;
    }
    PrintHeader(data[current]);
  }
}

}  // namespace fidl_codec
