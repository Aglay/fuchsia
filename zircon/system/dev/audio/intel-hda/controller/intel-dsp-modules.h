// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CONTROLLER_INTEL_DSP_MODULES_H_
#define ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CONTROLLER_INTEL_DSP_MODULES_H_

#include <zircon/types.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <fbl/span.h>
#include <intel-hda/utils/intel-audio-dsp-ipc.h>
#include <intel-hda/utils/status.h>
#include <intel-hda/utils/status_or.h>

#include "intel-dsp-ipc.h"

namespace audio {
namespace intel_hda {

using DspModuleType = uint16_t;

// Name of a module instance.
struct DspModuleId {
  DspModuleType type;  // Type of the module.
  uint8_t id;          // Instance number of the module.
};

// Name of a pipeline instance.
struct DspPipelineId {
  uint8_t id;
};

// Information about a DSP module instance.
struct DspModule {
  DspModuleType type;
  std::vector<uint8_t> data;
};

// DspModuleController manages set up of modules and pipelines, pipeline states,
// and module/pipeline ID allocation.
//
// Thread compatible.
class DspModuleController {
 public:
  DspModuleController(DspChannel* ipc);

  // Create a pipeline.
  //
  // Return ths ID of the created pipeline on success.
  StatusOr<DspPipelineId> CreatePipeline(uint8_t priority, uint16_t memory_pages, bool low_power);

  // Create an instance of the module "type" in the given pipeline.
  //
  // Returns the ID of the created module on success.
  StatusOr<DspModuleId> CreateModule(DspModuleType type, DspPipelineId parent_pipeline,
                                     ProcDomain scheduling_domain, fbl::Span<const uint8_t> data);

  // Connect an output pin of one module to the input pin of another.
  Status BindModules(DspModuleId source_module, uint8_t src_output_pin, DspModuleId dest_module,
                     uint8_t dest_input_pin);

  // Enable/disable the given pipeline.
  Status SetPipelineState(DspPipelineId pipeline, PipelineState state, bool sync_stop_start);

 private:
  // Allocate an instance ID for module of type |type|.
  StatusOr<uint8_t> AllocateInstanceId(DspModuleType type);

  // Number of instances of each module type that have been created.
  std::unordered_map<DspModuleType, uint8_t> allocated_instances_;

  // Number of pipelines created.
  uint8_t pipelines_allocated_ = 0;

  // Connection to the DSP. Owned elsewhere.
  DspChannel* channel_;
};

// Construct a simple pipeline, consisting of a series of modules in
// a straight line:
//
//    A --> B --> C --> D
//
// Modules should be listed in source to sink order. Each module will be
// joined to the previous module, connecting output pin 0 to input pin 0.
StatusOr<DspPipelineId> CreateSimplePipeline(DspModuleController* controller,
                                             std::initializer_list<DspModule> modules);

// Library & Module Management IPC
zx_status_t DspLargeConfigGet(DspChannel* ipc, uint16_t module_id, uint8_t instance_id,
                              BaseFWParamType large_param_id, fbl::Span<uint8_t> buffer,
                              size_t* bytes_received);

}  // namespace intel_hda
}  // namespace audio

#endif  // ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CONTROLLER_INTEL_DSP_MODULES_H_
