// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/public/types.h"

#include "peridot/lib/convert/convert.h"
#include "peridot/lib/util/ptr.h"

namespace storage {
namespace {
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::unique_ptr<T>& ptr) {
  if (ptr) {
    return os << *ptr;
  }
  return os;
}
}  // namespace

ObjectDigest::ObjectDigest() = default;
ObjectDigest::ObjectDigest(std::string digest) : digest_(std::move(digest)) {}
ObjectDigest::ObjectDigest(const flatbuffers::Vector<uint8_t>* digest)
    : ObjectDigest::ObjectDigest(convert::ToString(digest)) {}

ObjectDigest::ObjectDigest(const ObjectDigest&) = default;
ObjectDigest& ObjectDigest::operator=(const ObjectDigest&) = default;
ObjectDigest::ObjectDigest(ObjectDigest&&) = default;
ObjectDigest& ObjectDigest::operator=(ObjectDigest&&) = default;

bool ObjectDigest::IsValid() const { return digest_.has_value(); }
const std::string& ObjectDigest::Serialize() const {
  FXL_DCHECK(IsValid());
  return digest_.value();
}

bool operator==(const ObjectDigest& lhs, const ObjectDigest& rhs) {
  return lhs.digest_ == rhs.digest_;
}
bool operator!=(const ObjectDigest& lhs, const ObjectDigest& rhs) { return !(lhs == rhs); }
bool operator<(const ObjectDigest& lhs, const ObjectDigest& rhs) {
  return lhs.digest_ < rhs.digest_;
}

std::ostream& operator<<(std::ostream& os, const ObjectDigest& e) {
  return os << (e.IsValid() ? convert::ToHex(e.Serialize()) : "invalid-digest");
}

ObjectIdentifier::ObjectIdentifier()
    : key_index_(0), object_digest_(ObjectDigest()), token_(nullptr) {}

ObjectIdentifier::ObjectIdentifier(uint32_t key_index, ObjectDigest object_digest,
                                   std::shared_ptr<ObjectIdentifier::Token> token)
    : key_index_(key_index), object_digest_(std::move(object_digest)), token_(std::move(token)) {}

ObjectIdentifier::ObjectIdentifier(const ObjectIdentifier&) = default;
ObjectIdentifier::ObjectIdentifier(ObjectIdentifier&&) = default;
ObjectIdentifier& ObjectIdentifier::operator=(const ObjectIdentifier&) = default;
ObjectIdentifier& ObjectIdentifier::operator=(ObjectIdentifier&&) = default;

// The destructor must be defined even if purely virtual for destruction to work.
ObjectIdentifier::Token::~Token() = default;

bool operator==(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs) {
  return std::tie(lhs.key_index_, lhs.object_digest_) ==
         std::tie(rhs.key_index_, rhs.object_digest_);
}

bool operator!=(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs) { return !(lhs == rhs); }

bool operator<(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs) {
  return std::tie(lhs.key_index_, lhs.object_digest_) <
         std::tie(rhs.key_index_, rhs.object_digest_);
}

std::ostream& operator<<(std::ostream& os, const ObjectIdentifier& e) {
  return os << "ObjectIdentifier{key_index: " << e.key_index()
            << ", object_digest: " << e.object_digest() << "}";
}

bool operator==(const Entry& lhs, const Entry& rhs) {
  return std::tie(lhs.key, lhs.object_identifier, lhs.priority, lhs.entry_id) ==
         std::tie(rhs.key, rhs.object_identifier, rhs.priority, rhs.entry_id);
}

bool operator!=(const Entry& lhs, const Entry& rhs) { return !(lhs == rhs); }

std::ostream& operator<<(std::ostream& os, const Entry& e) {
  return os << "Entry{key: " << e.key << ", value: " << e.object_identifier
            << ", priority: " << (e.priority == KeyPriority::EAGER ? "EAGER" : "LAZY")
            << ", entry_id: " << convert::ToHex(e.entry_id) << "}";
}

bool operator==(const EntryChange& lhs, const EntryChange& rhs) {
  return std::tie(lhs.deleted, lhs.entry) == std::tie(rhs.deleted, rhs.entry);
}

bool operator!=(const EntryChange& lhs, const EntryChange& rhs) { return !(lhs == rhs); }

std::ostream& operator<<(std::ostream& os, const EntryChange& e) {
  return os << "EntryChange{entry: " << e.entry << ", deleted: " << e.deleted << "}";
}

bool operator==(const ThreeWayChange& lhs, const ThreeWayChange& rhs) {
  return util::EqualPtr(lhs.base, rhs.base) && util::EqualPtr(lhs.left, rhs.left) &&
         util::EqualPtr(lhs.right, rhs.right);
}

bool operator!=(const ThreeWayChange& lhs, const ThreeWayChange& rhs) { return !(lhs == rhs); }

std::ostream& operator<<(std::ostream& os, const ThreeWayChange& e) {
  return os << "ThreeWayChange{base: " << e.base << ", left: " << e.left << ", right: " << e.right
            << "}";
}

}  // namespace storage
