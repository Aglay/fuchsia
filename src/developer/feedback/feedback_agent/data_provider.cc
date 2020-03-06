// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_agent/data_provider.h"

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/fit/promise.h>
#include <lib/fit/result.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <memory>

#include "src/developer/feedback/feedback_agent/annotations/aliases.h"
#include "src/developer/feedback/feedback_agent/attachments/aliases.h"
#include "src/developer/feedback/feedback_agent/attachments/screenshot_ptr.h"
#include "src/developer/feedback/feedback_agent/attachments/util.h"
#include "src/developer/feedback/feedback_agent/config.h"
#include "src/developer/feedback/feedback_agent/image_conversion.h"
#include "src/lib/fsl/vmo/sized_vmo.h"
#include "src/lib/fsl/vmo/strings.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/lib/timekeeper/system_clock.h"

namespace feedback {
namespace {

using fuchsia::feedback::Data;
using fuchsia::feedback::ImageEncoding;
using fuchsia::feedback::Screenshot;

const char kConfigPath[] = "/pkg/data/config.json";

// Timeout for a single asynchronous piece of data, e.g., syslog collection.
const zx::duration kDataTimeout = zx::sec(30);
// Timeout for requesting the screenshot from Scenic.
const zx::duration kScreenshotTimeout = zx::sec(10);

}  // namespace

std::unique_ptr<DataProvider> DataProvider::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services) {
  Config config;

  if (const zx_status_t status = ParseConfig(kConfigPath, &config); status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to read config file at " << kConfigPath;

    FX_LOGS(FATAL) << "Failed to set up data provider";
    return nullptr;
  }

  return std::make_unique<DataProvider>(dispatcher, std::move(services), config);
}

DataProvider::DataProvider(async_dispatcher_t* dispatcher,
                           std::shared_ptr<sys::ServiceDirectory> services, const Config& config,
                           std::unique_ptr<timekeeper::Clock> clock)
    : dispatcher_(dispatcher),
      services_(services),
      config_(config),
      cobalt_(dispatcher_, services_, std::move(clock)),
      executor_(dispatcher),
      datastore_(dispatcher_, services_, &cobalt_, kDataTimeout, config_.annotation_allowlist,
                 config_.attachment_allowlist) {}

namespace {

std::vector<fuchsia::feedback::Annotation> ToAnnotationVector(const Annotations& annotations) {
  std::vector<fuchsia::feedback::Annotation> vec;
  for (const auto& [key, value] : annotations) {
    fuchsia::feedback::Annotation annotation;
    annotation.key = key;
    annotation.value = value;
    vec.push_back(std::move(annotation));
  }
  return vec;
}

std::vector<fuchsia::feedback::Attachment> ToAttachmentVector(const Attachments& attachments) {
  std::vector<fuchsia::feedback::Attachment> vec;
  for (const auto& [key, value] : attachments) {
    fsl::SizedVmo vmo;
    if (!fsl::VmoFromString(value, &vmo)) {
      FX_LOGS(ERROR) << fxl::StringPrintf("Failed to convert attachment %s to VMO", key.c_str());
      continue;
    }

    fuchsia::feedback::Attachment attachment;
    attachment.key = key;
    attachment.value = std::move(vmo).ToTransport();
    vec.push_back(std::move(attachment));
  }
  return vec;
}

}  // namespace

void DataProvider::GetData(GetDataCallback callback) {
  FX_CHECK(!shut_down_);

  const uint64_t timer_id = cobalt_.StartTimer();

  auto promise =
      fit::join_promises(datastore_.GetAnnotations(), datastore_.GetAttachments())
          .and_then([](std::tuple<fit::result<Annotations>, fit::result<Attachments>>&
                           annotations_and_attachments) {
            Data data;
            std::vector<fuchsia::feedback::Attachment> attachments;

            auto& annotations_or_error = std::get<0>(annotations_and_attachments);
            if (annotations_or_error.is_ok()) {
              data.set_annotations(ToAnnotationVector(annotations_or_error.take_value()));
            } else {
              FX_LOGS(WARNING) << "Failed to retrieve any annotations";
            }

            auto& attachments_or_error = std::get<1>(annotations_and_attachments);
            if (attachments_or_error.is_ok()) {
              attachments = ToAttachmentVector(attachments_or_error.take_value());
            } else {
              FX_LOGS(WARNING) << "Failed to retrieve any attachments";
            }

            // We also add the annotations as a single extra attachment.
            // This is useful for clients that surface the annotations differentily in the UI
            // but still want all the annotations to be easily downloadable in one file.
            if (data.has_annotations()) {
              AddAnnotationsAsExtraAttachment(data.annotations(), &attachments);
            }

            // We bundle the attachments into a single attachment.
            // This is useful for most clients that want to pass around a single bundle.
            if (!attachments.empty()) {
              fuchsia::feedback::Attachment bundle;
              if (BundleAttachments(attachments, &bundle)) {
                data.set_attachment_bundle(std::move(bundle));
              }
            }

            return fit::ok(std::move(data));
          })
          .or_else([]() { return fit::error(ZX_ERR_INTERNAL); })
          .then([this, callback = std::move(callback),
                 timer_id](fit::result<Data, zx_status_t>& result) {
            if (result.is_error()) {
              cobalt_.LogElapsedTime(BugreportGenerationFlow::kFailure, timer_id);
            } else {
              cobalt_.LogElapsedTime(BugreportGenerationFlow::kSuccess, timer_id);
            }
            callback(std::move(result));
          });

  executor_.schedule_task(std::move(promise));
}

void DataProvider::GetScreenshot(ImageEncoding encoding, GetScreenshotCallback callback) {
  FX_CHECK(!shut_down_);
  auto promise = TakeScreenshot(dispatcher_, services_, kScreenshotTimeout, &cobalt_)
                     .and_then([encoding](fuchsia::ui::scenic::ScreenshotData& raw_screenshot)
                                   -> fit::result<Screenshot> {
                       Screenshot screenshot;
                       screenshot.dimensions_in_px.height = raw_screenshot.info.height;
                       screenshot.dimensions_in_px.width = raw_screenshot.info.width;
                       switch (encoding) {
                         case ImageEncoding::PNG:
                           if (!RawToPng(raw_screenshot.data, raw_screenshot.info.height,
                                         raw_screenshot.info.width, raw_screenshot.info.stride,
                                         raw_screenshot.info.pixel_format, &screenshot.image)) {
                             FX_LOGS(ERROR) << "Failed to convert raw screenshot to PNG";
                             return fit::error();
                           }
                           break;
                       }
                       return fit::ok(std::move(screenshot));
                     })
                     .then([callback = std::move(callback)](fit::result<Screenshot>& result) {
                       if (!result.is_ok()) {
                         callback(/*screenshot=*/nullptr);
                       } else {
                         callback(std::make_unique<Screenshot>(result.take_value()));
                       }
                     });

  executor_.schedule_task(std::move(promise));
}

void DataProvider::Shutdown() {
  shut_down_ = true;
  cobalt_.Shutdown();
}

}  // namespace feedback
