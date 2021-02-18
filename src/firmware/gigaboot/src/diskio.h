// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_FIRMWARE_GIGABOOT_SRC_DISKIO_H_
#define SRC_FIRMWARE_GIGABOOT_SRC_DISKIO_H_

#include <zircon/compiler.h>
#include <zircon/hw/gpt.h>

#include <efi/protocol/disk-io.h>
#include <efi/system-table.h>

__BEGIN_CDECLS

typedef struct {
  efi_disk_io_protocol* io;
  efi_handle h;
  efi_boot_services* bs;
  efi_handle img;
  uint64_t first;
  uint64_t last;
  uint32_t blksz;
  uint32_t id;
} disk_t;

efi_status read_partition(efi_handle img, efi_system_table* sys, const uint8_t* guid_value,
                          const char* guid_name, uint64_t offset, unsigned char* data, size_t size);

efi_status write_partition(efi_handle img, efi_system_table* sys, const uint8_t* guid_value,
                           const char* guid_name, uint64_t offset, const unsigned char* data,
                           size_t size);

void* image_load_from_disk(efi_handle img, efi_system_table* sys, size_t* sz,
                           const uint8_t* guid_value, const char* guid_name);

// Returns true if the disk device that was used to load the bootloader
// is connected via USB.
bool is_booting_from_usb(efi_handle img, efi_system_table* sys);

// Find the disk device that was used to load the boot loader.
// Returns 0 on success and fills in the disk pointer, -1 otherwise.
int disk_find_boot(efi_handle img, efi_system_table* sys, bool verbose, disk_t* disk);

// Reads the GPT from the front of |disk| and finds the requested partition.
//
// The matcher will find a partition which satisfies all of the given |type|,
// |guid|, and |name| parameters.
//
// disk: the disk to search.
// verbose: true to print additional debug info.
// type: partition type GUID, or NULL to match any.
// guid: partition GUID, or NULL to match any.
// name: UTF-8 partition name, or NULL to match any.
// partition: on success, filled with the resulting GPT partition entry. Note
//            that .first and .last are in block units, and .name is UTF-16.
//
// Returns 0 on success, -1 if no partitions or multiple partitions match.
int disk_find_partition(const disk_t* disk, bool verbose, const uint8_t* type, const uint8_t* guid,
                        const char* name, gpt_entry_t* partition);

efi_status disk_write(disk_t* disk, size_t offset, void* data, size_t length);

// guid_value_from_name takes in a GUID name and puts the associated GUID value
// into value.
// Returns 0 on success, -1 if the guid_name was not found.
int guid_value_from_name(const char* guid_name, uint8_t* value);

__END_CDECLS

#endif  // SRC_FIRMWARE_GIGABOOT_SRC_DISKIO_H_
