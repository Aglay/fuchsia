// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/metadata.h"

#include <lib/zx/time.h>

#include <optional>
#include <set>

#include "src/developer/forensics/feedback_data/constants.h"
#include "src/developer/forensics/feedback_data/errors.h"
#include "src/developer/forensics/utils/cobalt/metrics.h"
#include "src/developer/forensics/utils/errors.h"

// TODO(fxbug.dev/57392): Move it back to //third_party once unification completes.
#include "zircon/third_party/rapidjson/include/rapidjson/document.h"
#include "zircon/third_party/rapidjson/include/rapidjson/prettywriter.h"
#include "zircon/third_party/rapidjson/include/rapidjson/rapidjson.h"
#include "zircon/third_party/rapidjson/include/rapidjson/stringbuffer.h"

namespace forensics {
namespace feedback_data {
namespace {

using namespace rapidjson;

std::set<std::string> kUtcMonotonicDifferenceAllowlist = {
    kAttachmentInspect,
    kAttachmentLogKernel,
    kAttachmentLogSystem,
};

std::string ToString(const enum AttachmentValue::State state) {
  switch (state) {
    case AttachmentValue::State::kComplete:
      return "complete";
    case AttachmentValue::State::kPartial:
      return "partial";
    case AttachmentValue::State::kMissing:
      return "missing";
  }
}

// Create a complete list set of annotations from the collected annotations and the allowlist.
Annotations AllAnnotations(const AnnotationKeys& allowlist,
                           const ::fit::result<Annotations>& annotations_result) {
  Annotations all_annotations;
  if (annotations_result.is_ok()) {
    all_annotations.insert(annotations_result.value().cbegin(), annotations_result.value().cend());
  }

  for (const auto& key : allowlist) {
    if (all_annotations.find(key) == all_annotations.end()) {
      // There is an annotation in the allowlist that was not produced by any provider. This
      // indicates a logical error on the Feedback-side.
      all_annotations.insert({key, AnnotationOr(Error::kLogicError)});
    }
  }

  return all_annotations;
}

// Create a complete list set of attachments from the collected attachments and the allowlist.
Attachments AllAttachments(const AttachmentKeys& allowlist,
                           const ::fit::result<Attachments>& attachments_result) {
  Attachments all_attachments;
  if (attachments_result.is_ok()) {
    // Because attachments can contain large blobs of text and we only care about the state of the
    // attachment and its associated error, we don't copy the value of the attachment.
    for (const auto& [k, v] : attachments_result.value()) {
      switch (v.State()) {
        case AttachmentValue::State::kComplete:
          all_attachments.insert({k, AttachmentValue("")});
          break;
        case AttachmentValue::State::kPartial:
          all_attachments.insert({k, AttachmentValue("", v.Error())});
          break;
        case AttachmentValue::State::kMissing:
          all_attachments.insert({k, v});
          break;
      }
    }
  }

  for (const auto& key : allowlist) {
    if (all_attachments.find(key) == all_attachments.end()) {
      all_attachments.insert({key, AttachmentValue(Error::kLogicError)});
    }
  }

  return all_attachments;
}

void AddUtcMonotonicDifference(const std::optional<zx::duration>& utc_monotonic_difference,
                               Value* file, Document::AllocatorType& allocator) {
  if (!utc_monotonic_difference.has_value() || !file->IsObject() ||
      file->HasMember("utc_monotonic_difference_nanos")) {
    return;
  }

  file->AddMember("utc_monotonic_difference_nanos",
                  Value().SetInt64(utc_monotonic_difference.value().get()), allocator);
}

void AddAttachments(const AttachmentKeys& attachment_allowlist,
                    const ::fit::result<Attachments>& attachments_result,
                    const std::optional<zx::duration> utc_monotonic_difference,
                    Document* metadata_json) {
  if (attachment_allowlist.empty()) {
    return;
  }

  auto& allocator = metadata_json->GetAllocator();
  auto MakeValue = [&allocator](const std::string& v) { return Value(v, allocator); };

  for (const auto& [name, v] : AllAttachments(attachment_allowlist, attachments_result)) {
    Value file(kObjectType);

    file.AddMember("state", MakeValue(ToString(v.State())), allocator);
    if (v.HasError()) {
      file.AddMember("error", MakeValue(ToReason(v.Error())), allocator);
    }

    if (kUtcMonotonicDifferenceAllowlist.find(name) != kUtcMonotonicDifferenceAllowlist.end()) {
      AddUtcMonotonicDifference(utc_monotonic_difference, &file, allocator);
    }

    (*metadata_json)["files"].AddMember(MakeValue(name), file, allocator);
  }
}

void AddAnnotationsJson(const AnnotationKeys& annotation_allowlist,
                        const ::fit::result<Annotations>& annotations_result,
                        const bool missing_non_platform_annotations, Document* metadata_json) {
  const Annotations all_annotations = AllAnnotations(annotation_allowlist, annotations_result);

  bool has_non_platform = all_annotations.size() > annotation_allowlist.size();
  if (annotation_allowlist.empty() && !(has_non_platform || missing_non_platform_annotations)) {
    return;
  }

  auto& allocator = metadata_json->GetAllocator();
  auto MakeValue = [&allocator](const std::string& v) { return Value(v, allocator); };

  Value present(kArrayType);
  Value missing(kObjectType);

  size_t num_present_platform = 0u;
  size_t num_missing_platform = 0u;
  for (const auto& [k, v] : all_annotations) {
    if (annotation_allowlist.find(k) == annotation_allowlist.end()) {
      continue;
    }

    Value key(MakeValue(k));
    if (v.HasValue()) {
      present.PushBack(key, allocator);
      ++num_present_platform;
    } else {
      missing.AddMember(key, MakeValue(ToReason(v.Error())), allocator);
      ++num_missing_platform;
    }
  }

  if (has_non_platform || missing_non_platform_annotations) {
    if (!missing_non_platform_annotations) {
      present.PushBack("non-platform annotations", allocator);
    } else {
      missing.AddMember("non-platform annotations", "too many non-platfrom annotations added",
                        allocator);
    }
  }

  Value state;
  if (num_present_platform == annotation_allowlist.size() && !missing_non_platform_annotations) {
    state = "complete";
  } else if (num_missing_platform == annotation_allowlist.size() && !has_non_platform &&
             missing_non_platform_annotations) {
    state = "missing";
  } else {
    state = "partial";
  }

  Value annotations_json(kObjectType);
  annotations_json.AddMember("state", state, allocator);
  annotations_json.AddMember("missing annotations", missing, allocator);
  annotations_json.AddMember("present annotations", present, allocator);

  (*metadata_json)["files"].AddMember("annotations.json", annotations_json, allocator);
}

}  // namespace

Metadata::Metadata(std::shared_ptr<sys::ServiceDirectory> services, timekeeper::Clock* clock,
                   const AnnotationKeys& annotation_allowlist,
                   const AttachmentKeys& attachment_allowlist)
    : annotation_allowlist_(annotation_allowlist),
      attachment_allowlist_(attachment_allowlist),
      utc_provider_(services, clock) {}

std::string Metadata::MakeMetadata(const ::fit::result<Annotations>& annotations_result,
                                   const ::fit::result<Attachments>& attachments_result,
                                   bool missing_non_platform_annotations) {
  Document metadata_json(kObjectType);
  auto& allocator = metadata_json.GetAllocator();

  auto MetadataString = [&metadata_json]() {
    StringBuffer buffer;
    PrettyWriter<StringBuffer> writer(buffer);

    metadata_json.Accept(writer);

    return std::string(buffer.GetString());
  };

  // Insert all top-level fields
  metadata_json.AddMember("snapshot_version", Value(SnapshotVersion::kString, allocator),
                          allocator);
  metadata_json.AddMember("metadata_version", Value(Metadata::kVersion, allocator), allocator);
  metadata_json.AddMember("files", Value(kObjectType), allocator);

  const bool has_non_platform_annotations =
      annotations_result.is_ok() &&
      annotations_result.value().size() > annotation_allowlist_.size();

  if (annotation_allowlist_.empty() && attachment_allowlist_.empty() &&
      !has_non_platform_annotations && !missing_non_platform_annotations) {
    return MetadataString();
  }

  const auto utc_monotonic_difference = utc_provider_.CurrentUtcMonotonicDifference();

  AddAttachments(attachment_allowlist_, attachments_result, utc_monotonic_difference,
                 &metadata_json);
  AddAnnotationsJson(annotation_allowlist_, annotations_result, missing_non_platform_annotations,
                     &metadata_json);

  return MetadataString();
}

}  // namespace feedback_data
}  // namespace forensics
