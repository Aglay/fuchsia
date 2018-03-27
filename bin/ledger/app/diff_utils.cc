// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/app/diff_utils.h"

#include <limits>
#include <memory>
#include <vector>

#include "garnet/lib/callback/waiter.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/functional/make_copyable.h"
#include "peridot/bin/ledger/app/fidl/serialization_size.h"
#include "peridot/bin/ledger/app/page_utils.h"
#include "peridot/bin/ledger/storage/public/object.h"
#include "peridot/lib/util/ptr.h"

namespace ledger {
namespace diff_utils {
namespace {
// Returns the key of a storage::ThreeWayChange object. This key is guaranteed
// to be unique.
const std::string& GetKey(const storage::ThreeWayChange& change) {
  if (change.base) {
    return change.base->key;
  }
  if (change.left) {
    return change.left->key;
  }
  return change.right->key;
}

// Constructs a ValuePtr object from an entry. The contents of the ValuePtr will
// be provided through the |waiter|.
ValuePtr GetValueFromEntry(
    storage::PageStorage* const storage,
    const std::unique_ptr<storage::Entry>& entry,
    std::function<void(Status, fsl::SizedVmo)> callback) {
  if (!entry) {
    callback(Status::OK, fsl::SizedVmo());
    return nullptr;
  }
  ValuePtr value = Value::New();
  switch (entry->priority) {
    case storage::KeyPriority::EAGER:
      value->priority = Priority::EAGER;
      break;
    case storage::KeyPriority::LAZY:
      value->priority = Priority::LAZY;
      break;
  }
  PageUtils::ResolveObjectIdentifierAsBuffer(
      storage, entry->object_identifier, 0u,
      std::numeric_limits<int64_t>::max(),
      storage::PageStorage::Location::LOCAL, Status::OK, std::move(callback));
  return value;
}

// Returns true if the change is automatically mergeable, ie. is not
// conflicting.
bool IsMergeable(const storage::ThreeWayChange& change) {
  return util::EqualPtr(change.base, change.left) ||
         util::EqualPtr(change.base, change.right) ||
         util::EqualPtr(change.left, change.right);
}
}  // namespace

void ComputePageChange(
    storage::PageStorage* storage,
    const storage::Commit& base,
    const storage::Commit& other,
    std::string prefix_key,
    std::string min_key,
    PaginationBehavior pagination_behavior,
    std::function<void(Status, std::pair<PageChangePtr, std::string>)>
        callback) {
  struct Context {
    // The PageChangePtr to be returned through the callback.
    PageChangePtr page_change = PageChange::New();
    // The serialization size of all entries.
    size_t fidl_size = fidl_serialization::kPageChangeHeaderSize;
    // The number of handles.
    size_t handles_count = 0u;
    // The next token to be returned through the callback.
    std::string next_token = "";
  };

  auto waiter = callback::Waiter<Status, fsl::SizedVmo>::Create(Status::OK);

  auto context = std::make_unique<Context>();
  context->page_change->timestamp = other.GetTimestamp();
  context->page_change->changed_entries = fidl::VectorPtr<EntryPtr>::New(0);
  context->page_change->deleted_keys =
      fidl::VectorPtr<fidl::VectorPtr<uint8_t>>::New(0);

  if (min_key < prefix_key) {
    min_key = prefix_key;
  }

  // |on_next| is called for each change on the diff
  auto on_next = [storage, waiter, prefix_key = std::move(prefix_key),
                  context = context.get(),
                  pagination_behavior](storage::EntryChange change) {
    if (!PageUtils::MatchesPrefix(change.entry.key, prefix_key)) {
      return false;
    }
    size_t entry_size =
        change.deleted
            ? fidl_serialization::GetByteArraySize(change.entry.key.size())
            : fidl_serialization::GetEntrySize(change.entry.key.size());
    size_t entry_handle_count = change.deleted ? 0 : 1;
    if (pagination_behavior == PaginationBehavior::BY_SIZE &&
        (context->fidl_size + entry_size >
             fidl_serialization::kMaxInlineDataSize ||
         context->handles_count + entry_handle_count >
             fidl_serialization::kMaxMessageHandles

         )) {
      context->next_token = change.entry.key;
      return false;
    }
    context->fidl_size += entry_size;
    context->handles_count += entry_handle_count;
    if (change.deleted) {
      context->page_change->deleted_keys.push_back(
          convert::ToArray(change.entry.key));
      return true;
    }

    EntryPtr entry = Entry::New();
    entry->key = convert::ToArray(change.entry.key);
    entry->priority = change.entry.priority == storage::KeyPriority::EAGER
                          ? Priority::EAGER
                          : Priority::LAZY;
    context->page_change->changed_entries.push_back(std::move(entry));
    PageUtils::ResolveObjectIdentifierAsBuffer(
        storage, change.entry.object_identifier, 0u,
        std::numeric_limits<int64_t>::max(),
        storage::PageStorage::Location::LOCAL, Status::OK,
        waiter->NewCallback());
    return true;
  };

  // |on_done| is called when the full diff is computed.
  auto on_done = fxl::MakeCopyable([waiter = std::move(waiter),
                                    context = std::move(context),
                                    callback = std::move(callback)](
                                       storage::Status status) mutable {
    if (status != storage::Status::OK) {
      FXL_LOG(ERROR) << "Unable to compute diff for PageChange: " << status;
      callback(PageUtils::ConvertStatus(status), std::make_pair(nullptr, ""));
      return;
    }
    if (context->page_change->changed_entries->empty()) {
      if (context->page_change->deleted_keys->empty()) {
        callback(Status::OK, std::make_pair(nullptr, ""));
      } else {
        callback(Status::OK,
                 std::make_pair(std::move(context->page_change), ""));
      }
      return;
    }

    // We need to retrieve the values for each changed key/value pair in order
    // to send it inside the PageChange object. |waiter| collates these
    // asynchronous calls and |result_callback| processes them.
    auto result_callback = fxl::MakeCopyable([context = std::move(context),
                                              callback = std::move(callback)](
                                                 Status status,
                                                 std::vector<fsl::SizedVmo>
                                                     results) mutable {
      if (status != Status::OK) {
        FXL_LOG(ERROR)
            << "Error while reading changed values when computing PageChange: "
            << status;
        callback(status, std::make_pair(nullptr, ""));
        return;
      }
      FXL_DCHECK(results.size() ==
                 context->page_change->changed_entries->size());
      for (size_t i = 0; i < results.size(); i++) {
        context->page_change->changed_entries->at(i)->value =
            std::move(results[i]).ToTransport();
      }
      callback(Status::OK, std::make_pair(std::move(context->page_change),
                                          std::move(context->next_token)));
    });
    waiter->Finalize(std::move(result_callback));
  });
  storage->GetCommitContentsDiff(base, other, std::move(min_key),
                                 std::move(on_next), std::move(on_done));
}

void ComputeThreeWayDiff(
    storage::PageStorage* storage,
    const storage::Commit& base,
    const storage::Commit& left,
    const storage::Commit& right,
    std::string prefix_key,
    std::string min_key,
    DiffType diff_type,
    std::function<void(Status,
                       std::pair<fidl::VectorPtr<DiffEntryPtr>, std::string>)>
        callback) {
  struct Context {
    // The array to be returned through the callback.
    fidl::VectorPtr<DiffEntryPtr> changes;
    // The serialization size of all entries.
    size_t fidl_size = fidl_serialization::kArrayHeaderSize;
    // The number of handles.
    size_t handles_count = 0u;
    // The next token to be returned through the callback.
    std::string next_token = "";
  };

  // This waiter collects the values (as VMOs) for all changes that will be
  // returned. As each |DiffEntry| struct has three values, we ensure that
  // values are always returned in a specific order (base, left, right). Some
  // values may be empty, to denote a lack of diff.
  auto waiter = callback::Waiter<Status, fsl::SizedVmo>::Create(Status::OK);

  auto context = std::make_unique<Context>();

  if (min_key < prefix_key) {
    min_key = prefix_key;
  }

  // |on_next| is called for each change on the diff
  auto on_next = [storage, waiter, prefix_key = std::move(prefix_key),
                  context = context.get(),
                  diff_type](storage::ThreeWayChange change) mutable {
    const std::string& key = GetKey(change);
    if (!PageUtils::MatchesPrefix(key, prefix_key)) {
      return false;
    }
    int number_of_values =
        bool(change.base) + bool(change.left) + bool(change.right);
    size_t diffentry_size =
        fidl_serialization::GetByteArraySize(key.size()) +
        number_of_values *
            (fidl_serialization::kHandleSize + fidl_serialization::kEnumSize);
    if (context->fidl_size + diffentry_size >
            fidl_serialization::kMaxInlineDataSize ||
        context->handles_count + number_of_values >
            fidl_serialization::kMaxMessageHandles) {
      context->next_token = key;
      // Stop the iteration as we are over the message capacity.
      return false;
    }

    if (diff_type == DiffType::CONFLICTING && IsMergeable(change)) {
      // We are not interested in this change, continue to the next one.
      return true;
    }

    context->fidl_size += diffentry_size;
    context->handles_count += number_of_values;

    DiffEntryPtr diff_entry = DiffEntry::New();
    diff_entry->key = convert::ToArray(key);
    diff_entry->base =
        GetValueFromEntry(storage, change.base, waiter->NewCallback());
    diff_entry->left =
        GetValueFromEntry(storage, change.left, waiter->NewCallback());
    diff_entry->right =
        GetValueFromEntry(storage, change.right, waiter->NewCallback());
    context->changes.push_back(std::move(diff_entry));
    return true;
  };

  // |on_done| is called when the full diff is computed.
  auto on_done = fxl::MakeCopyable([waiter = std::move(waiter),
                                    context = std::move(context),
                                    callback = std::move(callback)](
                                       storage::Status status) mutable {
    if (status != storage::Status::OK) {
      FXL_LOG(ERROR) << "Unable to compute diff for PageChange: " << status;
      callback(PageUtils::ConvertStatus(status), std::make_pair(nullptr, ""));
      return;
    }
    if (context->changes->empty()) {
      callback(Status::OK, std::make_pair(nullptr, ""));
      return;
    }

    // We need to retrieve the values for each changed key/value pair in order
    // to send it inside the PageChange object. |waiter| collates these
    // asynchronous calls and |result_callback| processes them.
    auto result_callback = fxl::MakeCopyable([context = std::move(context),
                                              callback = std::move(callback)](
                                                 Status status,
                                                 std::vector<fsl::SizedVmo>
                                                     results) mutable {
      if (status != Status::OK) {
        FXL_LOG(ERROR)
            << "Error while reading changed values when computing PageChange: "
            << status;
        callback(status, std::make_pair(nullptr, ""));
        return;
      }
      FXL_DCHECK(results.size() == 3 * context->changes->size());
      for (size_t i = 0; i < context->changes->size(); i++) {
        if (results[3 * i]) {
          context->changes->at(i)->base->value =
              std::move(results[3 * i]).ToTransport();
        }
        if (results[3 * i + 1]) {
          context->changes->at(i)->left->value =
              std::move(results[3 * i + 1]).ToTransport();
        }
        if (results[3 * i + 2]) {
          context->changes->at(i)->right->value =
              std::move(results[3 * i + 2]).ToTransport();
        }
      }
      callback(Status::OK, std::make_pair(std::move(context->changes),
                                          std::move(context->next_token)));
    });
    waiter->Finalize(std::move(result_callback));
  });
  storage->GetThreeWayContentsDiff(base, left, right, std::move(min_key),
                                   std::move(on_next), std::move(on_done));
}

}  // namespace diff_utils
}  // namespace ledger
