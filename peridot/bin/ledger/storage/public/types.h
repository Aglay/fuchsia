// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_STORAGE_PUBLIC_TYPES_H_
#define PERIDOT_BIN_LEDGER_STORAGE_PUBLIC_TYPES_H_

#include <ostream>
#include <string>

#include <lib/fxl/strings/string_view.h>

#include "peridot/lib/convert/convert.h"

namespace storage {

using PageId = std::string;
using PageIdView = convert::ExtendedStringView;
using CommitId = std::string;
using CommitIdView = convert::ExtendedStringView;

// The type of object.
// Ledger stores user created content on BTrees, where the nodes (TREE_NODE
// objects) store the user-created keys and references to the user-created
// values. The content of the values is (usually) stored into separate BLOB
// objects.
// See ledger/storage/impl/btree for more details.
enum class ObjectType {
  // A |TreeNode| object.
  TREE_NODE,
  // An opaque sequence of bytes. Currently used to store values.
  BLOB,
};

// The digest of an object.
// This class is a container for an object digest, treated as an opaque blob. It
// is not responsible for computing or validating the digest; see
// storage/impl/object_digest.h for such functions.
class ObjectDigest {
 public:
  // Builds an invalid object digest. Useful, eg., when returning a default
  // object upon error (with a failed status).
  ObjectDigest();

  // Builds a valid object digest whose value is equal to |digest|.
  explicit ObjectDigest(std::string digest);
  explicit ObjectDigest(const flatbuffers::Vector<uint8_t>* digest);

  ObjectDigest(const ObjectDigest&);
  ObjectDigest& operator=(const ObjectDigest&);
  ObjectDigest(ObjectDigest&&);
  ObjectDigest& operator=(ObjectDigest&&);

  // Returns whether this object represents a valid object digest.
  bool IsValid() const;

  // Returns the content of the object digest.
  // The reference is valid as long as this object. Must only be called if the
  // object is valid.
  const std::string& Serialize() const;

 private:
  friend bool operator==(const ObjectDigest& lhs, const ObjectDigest& rhs);
  friend bool operator<(const ObjectDigest& lhs, const ObjectDigest& rhs);

  std::optional<std::string> digest_;
};

bool operator==(const ObjectDigest& lhs, const ObjectDigest& rhs);
bool operator!=(const ObjectDigest& lhs, const ObjectDigest& rhs);
bool operator<(const ObjectDigest& lhs, const ObjectDigest& rhs);
std::ostream& operator<<(std::ostream& os, const ObjectDigest& e);

// The priority at which the key value is downloaded, and the cache policy.
enum class KeyPriority {
  EAGER,
  LAZY,
};

// The identifier of an object. This contains the digest of the object, as well
// as the information needed to hide its name and encrypt its content.
class ObjectIdentifier {
 public:
  ObjectIdentifier();
  ObjectIdentifier(uint32_t key_index, uint32_t deletion_scope_id,
                   ObjectDigest object_digest);

  ObjectIdentifier(const ObjectIdentifier&);
  ObjectIdentifier& operator=(const ObjectIdentifier&);
  ObjectIdentifier(ObjectIdentifier&&);
  ObjectIdentifier& operator=(ObjectIdentifier&&);

  uint32_t key_index() const { return key_index_; }
  uint32_t deletion_scope_id() const { return deletion_scope_id_; }
  const ObjectDigest& object_digest() const { return object_digest_; }

 private:
  friend bool operator==(const ObjectIdentifier&, const ObjectIdentifier&);
  friend bool operator<(const ObjectIdentifier&, const ObjectIdentifier&);

  uint32_t key_index_;
  uint32_t deletion_scope_id_;
  ObjectDigest object_digest_;
};

bool operator==(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs);
bool operator!=(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs);
bool operator<(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs);
std::ostream& operator<<(std::ostream& os, const ObjectIdentifier& e);

// An entry in a commit.
struct Entry {
  std::string key;
  ObjectIdentifier object_identifier;
  KeyPriority priority;
};

bool operator==(const Entry& lhs, const Entry& rhs);
bool operator!=(const Entry& lhs, const Entry& rhs);
std::ostream& operator<<(std::ostream& os, const Entry& e);

// A change between two commit contents.
struct EntryChange {
  Entry entry;
  bool deleted;
};

bool operator==(const EntryChange& lhs, const EntryChange& rhs);
bool operator!=(const EntryChange& lhs, const EntryChange& rhs);
std::ostream& operator<<(std::ostream& os, const EntryChange& e);

// A change between 3 commit contents.
struct ThreeWayChange {
  std::unique_ptr<Entry> base;
  std::unique_ptr<Entry> left;
  std::unique_ptr<Entry> right;
};

bool operator==(const ThreeWayChange& lhs, const ThreeWayChange& rhs);
bool operator!=(const ThreeWayChange& lhs, const ThreeWayChange& rhs);
std::ostream& operator<<(std::ostream& os, const ThreeWayChange& e);

enum class ChangeSource { LOCAL, P2P, CLOUD };
enum class IsObjectSynced : bool { NO, YES };

enum class JournalType { IMPLICIT, EXPLICIT };

enum class JournalContainsClearOperation { NO, YES };

enum class Status {
  // User visible status.
  OK,
  IO_ERROR,
  NOT_FOUND,

  // Internal status.
  FORMAT_ERROR,
  ILLEGAL_STATE,
  INTERNAL_IO_ERROR,
  INTERRUPTED,
  NOT_CONNECTED_ERROR,
  NO_SUCH_CHILD,
  OBJECT_DIGEST_MISMATCH,

  // Temporary status or status for tests.
  NOT_IMPLEMENTED,
};

fxl::StringView StatusToString(Status status);
std::ostream& operator<<(std::ostream& os, Status status);

}  // namespace storage
#endif  // PERIDOT_BIN_LEDGER_STORAGE_PUBLIC_TYPES_H_
