// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/intrusive_wavl_tree.h>
#include <fuzz-utils/string-map.h>

namespace fuzzing {

StringMap::StringMap() {
    iterator_ = elements_.end();
}

StringMap::~StringMap() {}

bool StringMap::is_empty() const {
    return elements_.is_empty();
}

size_t StringMap::size() const {
    return elements_.size();
}

const char* StringMap::get(const char* key) const {
    ZX_DEBUG_ASSERT(key);
    auto iterator = elements_.find(key);
    return iterator == elements_.end() ? nullptr : iterator->val.c_str();
}

void StringMap::set(const char* key, const char* val) {
    ZX_DEBUG_ASSERT(key);
    ZX_DEBUG_ASSERT(val);
    fbl::AllocChecker ac;
    fbl::unique_ptr<StringElement> element(new (&ac) StringElement());
    ZX_ASSERT(ac.check());
    element->key.Set(key, &ac);
    ZX_ASSERT(ac.check());
    element->val.Set(val, &ac);
    ZX_ASSERT(ac.check());
    elements_.insert_or_replace(fbl::move(element));
    iterator_ = elements_.end();
}

} // namespace fuzzing
