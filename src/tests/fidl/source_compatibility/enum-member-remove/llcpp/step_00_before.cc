// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/test/enummemberremove/llcpp/fidl.h>  // nogncheck
namespace fidl_test = llcpp::fidl::test::enummemberremove;

// [START contents]
fidl_test::wire::Color writer(std::string s) {
  if (s == "red") {
    return fidl_test::wire::Color::RED;
  } else if (s == "blue") {
    return fidl_test::wire::Color::BLUE;
  } else if (s == "yellow") {
    return fidl_test::wire::Color::YELLOW;
  } else {
    return fidl_test::wire::Color::Unknown();
  }
}

std::string reader(fidl_test::wire::Color color) {
  switch (color) {
    case fidl_test::wire::Color::RED:
      return "red";
    case fidl_test::wire::Color::BLUE:
      return "blue";
    case fidl_test::wire::Color::YELLOW:
      return "yellow";
    default:
      return "<unknown>";
  }
}
// [END contents]

int main(int argc, const char** argv) { return 0; }
