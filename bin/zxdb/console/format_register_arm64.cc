// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>

#include "garnet/bin/zxdb/client/register.h"
#include "garnet/bin/zxdb/common/err.h"
#include "garnet/bin/zxdb/console/format_register.h"
#include "garnet/bin/zxdb/console/format_register_arm64.h"
#include "garnet/bin/zxdb/console/format_table.h"
#include "garnet/bin/zxdb/console/output_buffer.h"
#include "garnet/bin/zxdb/console/string_formatters.h"
#include "garnet/lib/debug_ipc/helper/arch_arm64.h"
#include "lib/fxl/strings/string_printf.h"

using debug_ipc::RegisterCategory;
using debug_ipc::RegisterID;

namespace zxdb {

namespace {

#define FLAG_VALUE(value, shift, mask) (uint8_t)((value >> shift) & mask)

TextForegroundColor GetRowColor(size_t table_len) {
  return table_len % 2 == 0 ? TextForegroundColor::kDefault
                            : TextForegroundColor::kLightGray;
}

// Format General Registers
// -----------------------------------------------------

std::vector<OutputBuffer> DescribeCPSR(const Register& cpsr,
                                       TextForegroundColor color) {
  std::vector<OutputBuffer> result;
  result.emplace_back(color, RegisterIDToString(cpsr.id()));

  uint64_t value = cpsr.GetValue();

  // Hex value: rflags is a 32 bit value.
  result.emplace_back(color, fxl::StringPrintf("0x%08" PRIx64, value));

  // Decode individual flags.
  result.emplace_back(color,
                      fxl::StringPrintf("V=%d, C=%d, Z=%d, N=%d",
                                        ARM64_FLAG_VALUE(value, Cpsr, V),
                                        ARM64_FLAG_VALUE(value, Cpsr, C),
                                        ARM64_FLAG_VALUE(value, Cpsr, Z),
                                        ARM64_FLAG_VALUE(value, Cpsr, N)));

  return result;
}

std::vector<OutputBuffer> DescribeCPSRExtended(const Register& cpsr,
                                               TextForegroundColor color) {
  std::vector<OutputBuffer> result;
  result.reserve(3);
  result.emplace_back(OutputBuffer());
  result.emplace_back(OutputBuffer());

  uint64_t value = cpsr.GetValue();

  result.emplace_back(
      color,
      fxl::StringPrintf(
          "EL=%d, F=%d, I=%d, A=%d, D=%d, IL=%d, SS=%d, PAN=%d, UAO=%d",
          ARM64_FLAG_VALUE(value, Cpsr, EL), ARM64_FLAG_VALUE(value, Cpsr, F),
          ARM64_FLAG_VALUE(value, Cpsr, I), ARM64_FLAG_VALUE(value, Cpsr, A),
          ARM64_FLAG_VALUE(value, Cpsr, D), ARM64_FLAG_VALUE(value, Cpsr, IL),
          ARM64_FLAG_VALUE(value, Cpsr, SS), ARM64_FLAG_VALUE(value, Cpsr, PAN),
          ARM64_FLAG_VALUE(value, Cpsr, UAO)));
  return result;
}

void FormatGeneralRegisters(const FormatRegisterOptions& options,
                            const std::vector<Register>& registers,
                            OutputBuffer* out) {
  std::vector<std::vector<OutputBuffer>> rows;

  for (const Register& reg : registers) {
    auto color = GetRowColor(rows.size());
    if (reg.id() == RegisterID::kARMv8_cpsr) {
      rows.push_back(DescribeCPSR(reg, color));
      if (options.extended)
        rows.push_back(DescribeCPSRExtended(reg, color));
    } else {
      rows.push_back(DescribeRegister(reg, color));
    }
  }

  // Output the tables.
  if (!rows.empty()) {
    std::vector<ColSpec> colspecs({ColSpec(Align::kRight),
                                   ColSpec(Align::kRight, 0, std::string(), 1),
                                   ColSpec()});
    FormatTable(colspecs, rows, out);
  }
}

// ID_AA64DFR0_EL1 -------------------------------------------------------------

std::vector<OutputBuffer> FormatID_AA64FR0_EL1(const Register& reg,
                                               TextForegroundColor color) {
  std::vector<OutputBuffer> row;
  row.reserve(3);
  row.emplace_back(color, RegisterIDToString(reg.id()));

  uint64_t value = reg.GetValue();
  row.emplace_back(color, fxl::StringPrintf("0x%08" PRIx64, value));

  row.emplace_back(
      color, fxl::StringPrintf(
                 "DV=%d, TV=%d, PMUV=%d, BRP=%d, WRP=%d, CTX_CMP=%d, PMSV=%d",
                 ARM64_FLAG_VALUE(value, ID_AA64DFR0_EL1, DV),
                 ARM64_FLAG_VALUE(value, ID_AA64DFR0_EL1, TV),
                 ARM64_FLAG_VALUE(value, ID_AA64DFR0_EL1, PMUV),
                 // The register count values have 1 substracted to them.
                 ARM64_FLAG_VALUE(value, ID_AA64DFR0_EL1, BRP) + 1,
                 ARM64_FLAG_VALUE(value, ID_AA64DFR0_EL1, WRP) + 1,
                 ARM64_FLAG_VALUE(value, ID_AA64DFR0_EL1, CTX_CMP) + 1,
                 ARM64_FLAG_VALUE(value, ID_AA64DFR0_EL1, PMSV)));
  return row;
}

// MDSCR -----------------------------------------------------------------------

std::vector<OutputBuffer> FormatMDSCR(const Register& reg,
                                      TextForegroundColor color) {
  std::vector<OutputBuffer> row;
  row.reserve(3);
  row.emplace_back(color, RegisterIDToString(reg.id()));

  uint64_t value = reg.GetValue();
  row.emplace_back(color, fxl::StringPrintf("0x%08" PRIx64, value));

  row.emplace_back(color, fxl::StringPrintf(
        "SS=%d, TDCC=%d, KDE=%d, HDE=%d, MDE=%d, RAZ/WI=%d, TDA=%d, INTdis=%d, "
        "TXU=%d, RXO=%d, TXfull=%d, RXfull=%d",
        ARM64_FLAG_VALUE(value, MDSCR_EL1, SS),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, TDCC),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, KDE),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, HDE),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, MDE),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, RAZ_WI),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, TDA),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, INTdis),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, TXU),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, RXO),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, TXfull),
        ARM64_FLAG_VALUE(value, MDSCR_EL1, RXfull)));

  return row;
}

void FormatDebugRegisters(const FormatRegisterOptions& options,
                          const std::vector<Register>& registers,
                          OutputBuffer* out) {
  std::vector<std::vector<OutputBuffer>> rows;

  for (const Register& reg : registers) {
    auto color = GetRowColor(rows.size() + 1);
    if (reg.id() == RegisterID::kARMv8_id_aa64dfr0_el1) {
      rows.push_back(FormatID_AA64FR0_EL1(reg, color));
    } else if (reg.id() == RegisterID::kARMv8_mdscr_el1) {
      rows.push_back(FormatMDSCR(reg, color));
    } else {
      rows.push_back(DescribeRegister(reg, color));
    }
  }

  // Output the tables.
  if (!rows.empty()) {
    std::vector<ColSpec> colspecs({ColSpec(Align::kRight),
                                   ColSpec(Align::kRight, 0, std::string(), 1),
                                   ColSpec()});
    FormatTable(colspecs, rows, out);
  }
}

}  // namespace

bool FormatCategoryARM64(const FormatRegisterOptions& options,
                         debug_ipc::RegisterCategory::Type category,
                         const std::vector<Register>& registers,
                         OutputBuffer* out, Err* err) {
  switch (category) {
    case RegisterCategory::Type::kGeneral:
      FormatGeneralRegisters(options, registers, out);
      return true;
    case RegisterCategory::Type::kDebug:
      FormatDebugRegisters(options, registers, out);
      return true;
    default:
      return false;
  }

  return true;
}

}  // namespace zxdb
