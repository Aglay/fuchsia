// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ui/scenic/cpp/view_token_pair.h>

#include <zircon/assert.h>

namespace scenic {

ViewTokenPair NewViewTokenPair() {
  ViewTokenPair new_tokens;
  auto status = zx::eventpair::create(0u, &new_tokens.first.value,
                                      &new_tokens.second.value);
  // Assert even in non-debug builds, because eventpair creation can fail under
  // normal operation.  Failure can occur for example, if the job creation
  // policy governing this process forbids eventpair creation.
  //
  // It is unlikely that a well-behaved Scenic client would crash here; if you
  // hit this, it means something is very abnormal.
  ZX_ASSERT(status == ZX_OK);

  return new_tokens;
}

fuchsia::ui::views::ViewToken ToViewToken(zx::eventpair raw_token) {
  return fuchsia::ui::views::ViewToken({
      .value = std::move(raw_token),
  });
}

fuchsia::ui::views::ViewHolderToken ToViewHolderToken(zx::eventpair raw_token) {
  return fuchsia::ui::views::ViewHolderToken({
      .value = std::move(raw_token),
  });
}

}  // namespace scenic
