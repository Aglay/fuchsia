// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "lib/acpi_tables.h"

#include <assert.h>
#include <err.h>
#include <lib/acpi_lite.h>
#include <lib/acpi_lite/structures.h>
#include <trace.h>

#include <ktl/optional.h>
#include <ktl/span.h>
#include <lk/init.h>

#define LOCAL_TRACE 0

zx_status_t AcpiTables::cpu_count(uint32_t* cpu_count) const {
  uint32_t count = 0;
  auto visitor = [&count](acpi_lite::AcpiSubTableHeader* record) {
    acpi_lite::AcpiMadtLocalApicEntry* lapic = (acpi_lite::AcpiMadtLocalApicEntry*)record;
    if (!(lapic->flags & ACPI_MADT_FLAG_ENABLED)) {
      LTRACEF("Skipping disabled processor %02x\n", lapic->apic_id);
      return ZX_OK;
    }

    count++;
    return ZX_OK;
  };

  auto status = ForEachInMadt(ACPI_MADT_TYPE_LOCAL_APIC, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *cpu_count = count;
  return ZX_OK;
}

zx_status_t AcpiTables::cpu_apic_ids(uint32_t* apic_ids, uint32_t array_size,
                                     uint32_t* apic_id_count) const {
  uint32_t count = 0;
  auto visitor = [apic_ids, array_size, &count](acpi_lite::AcpiSubTableHeader* record) {
    acpi_lite::AcpiMadtLocalApicEntry* lapic = (acpi_lite::AcpiMadtLocalApicEntry*)record;
    if (!(lapic->flags & ACPI_MADT_FLAG_ENABLED)) {
      LTRACEF("Skipping disabled processor %02x\n", lapic->apic_id);
      return ZX_OK;
    }

    if (count >= array_size) {
      return ZX_ERR_INVALID_ARGS;
    }
    apic_ids[count++] = lapic->apic_id;
    return ZX_OK;
  };

  auto status = ForEachInMadt(ACPI_MADT_TYPE_LOCAL_APIC, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *apic_id_count = count;
  return ZX_OK;
}

zx_status_t AcpiTables::io_apic_count(uint32_t* io_apics_count) const {
  return NumInMadt(ACPI_MADT_TYPE_IO_APIC, io_apics_count);
}

zx_status_t AcpiTables::io_apics(io_apic_descriptor* io_apics, uint32_t array_size,
                                 uint32_t* io_apics_count) const {
  uint32_t count = 0;
  auto visitor = [io_apics, array_size, &count](acpi_lite::AcpiSubTableHeader* record) {
    acpi_lite::AcpiMadtIoApicEntry* io_apic = (acpi_lite::AcpiMadtIoApicEntry*)record;
    if (count >= array_size) {
      return ZX_ERR_INVALID_ARGS;
    }
    io_apics[count].apic_id = io_apic->io_apic_id;
    io_apics[count].paddr = io_apic->io_apic_address;
    io_apics[count].global_irq_base = io_apic->global_system_interrupt_base;
    count++;
    return ZX_OK;
  };
  auto status = ForEachInMadt(ACPI_MADT_TYPE_IO_APIC, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *io_apics_count = count;
  return ZX_OK;
}

zx_status_t AcpiTables::interrupt_source_overrides_count(uint32_t* overrides_count) const {
  return NumInMadt(ACPI_MADT_TYPE_INT_SOURCE_OVERRIDE, overrides_count);
}

zx_status_t AcpiTables::interrupt_source_overrides(io_apic_isa_override* overrides,
                                                   uint32_t array_size,
                                                   uint32_t* overrides_count) const {
  uint32_t count = 0;
  auto visitor = [overrides, array_size, &count](acpi_lite::AcpiSubTableHeader* record) {
    if (count >= array_size) {
      return ZX_ERR_INVALID_ARGS;
    }

    acpi_lite::AcpiMadtIntSourceOverrideEntry* iso =
        (acpi_lite::AcpiMadtIntSourceOverrideEntry*)record;

    ASSERT(iso->bus == 0);  // 0 means ISA, ISOs are only ever for ISA IRQs
    overrides[count].isa_irq = iso->source;
    overrides[count].remapped = true;
    overrides[count].global_irq = iso->global_sys_interrupt;

    uint32_t flags = iso->flags;
    uint32_t polarity = flags & ACPI_MADT_FLAG_POLARITY_MASK;
    uint32_t trigger = flags & ACPI_MADT_FLAG_TRIGGER_MASK;

    // Conforms below means conforms to the bus spec.  ISA is
    // edge triggered and active high.
    switch (polarity) {
      case ACPI_MADT_FLAG_POLARITY_CONFORMS:
      case ACPI_MADT_FLAG_POLARITY_HIGH:
        overrides[count].pol = IRQ_POLARITY_ACTIVE_HIGH;
        break;
      case ACPI_MADT_FLAG_POLARITY_LOW:
        overrides[count].pol = IRQ_POLARITY_ACTIVE_LOW;
        break;
      default:
        panic("Unknown IRQ polarity in override: %u\n", polarity);
    }

    switch (trigger) {
      case ACPI_MADT_FLAG_TRIGGER_CONFORMS:
      case ACPI_MADT_FLAG_TRIGGER_EDGE:
        overrides[count].tm = IRQ_TRIGGER_MODE_EDGE;
        break;
      case ACPI_MADT_FLAG_TRIGGER_LEVEL:
        overrides[count].tm = IRQ_TRIGGER_MODE_LEVEL;
        break;
      default:
        panic("Unknown IRQ trigger in override: %u\n", trigger);
    }

    count++;
    return ZX_OK;
  };

  auto status = ForEachInMadt(ACPI_MADT_TYPE_INT_SOURCE_OVERRIDE, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *overrides_count = count;
  return ZX_OK;
}

zx_status_t AcpiTables::NumInMadt(uint8_t type, uint32_t* element_count) const {
  uint32_t count = 0;
  auto visitor = [&count](acpi_lite::AcpiSubTableHeader* record) {
    count++;
    return ZX_OK;
  };

  auto status = ForEachInMadt(type, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *element_count = count;
  return ZX_OK;
}

template <typename V>
zx_status_t AcpiTables::ForEachInMadt(uint8_t type, V visitor) const {
  uintptr_t records_start, records_end;
  zx_status_t status = GetMadtRecordLimits(&records_start, &records_end);
  if (status != ZX_OK) {
    return status;
  }

  uintptr_t addr;
  acpi_lite::AcpiSubTableHeader* record_hdr;
  for (addr = records_start; addr < records_end; addr += record_hdr->length) {
    record_hdr = (acpi_lite::AcpiSubTableHeader*)addr;
    if (record_hdr->type == type) {
      status = visitor(record_hdr);
      if (status != ZX_OK) {
        return status;
      }
    }
  }

  if (addr != records_end) {
    TRACEF("malformed MADT\n");
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

zx_status_t AcpiTables::GetMadtRecordLimits(uintptr_t* start, uintptr_t* end) const {
  const acpi_lite::AcpiSdtHeader* table =
      GetTableBySignature(*tables_, acpi_lite::AcpiMadtTable::kSignature);
  if (table == nullptr) {
    TRACEF("could not find MADT\n");
    return ZX_ERR_NOT_FOUND;
  }
  acpi_lite::AcpiMadtTable* madt = (acpi_lite::AcpiMadtTable*)table;
  uintptr_t records_start = ((uintptr_t)madt) + sizeof(*madt);
  uintptr_t records_end = ((uintptr_t)madt) + madt->header.length;
  if (records_start >= records_end) {
    TRACEF("MADT wraps around address space\n");
    return ZX_ERR_INTERNAL;
  }
  // Shouldn't be too many records
  if (madt->header.length > 4096) {
    TRACEF("MADT suspiciously long: %u\n", madt->header.length);
    return ZX_ERR_INTERNAL;
  }
  *start = records_start;
  *end = records_end;
  return ZX_OK;
}

void AcpiTables::SetDefault(const AcpiTables* table) { default_ = table; }

const AcpiTables& AcpiTables::Default() {
  ASSERT_MSG(default_ != nullptr, "AcpiTables::SetDefault() must be called.");
  return *default_;
}
