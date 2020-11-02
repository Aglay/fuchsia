// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <diskio.h>
#include <stdio.h>
#include <string.h>
#include <zircon/hw/gpt.h>

#include <efi/protocol/block-io.h>
#include <efi/protocol/device-path-to-text.h>
#include <efi/protocol/device-path.h>
#include <efi/protocol/disk-io.h>
#include <efi/protocol/loaded-image.h>
#include <efi/protocol/simple-file-system.h>

#include "osboot.h"

static bool path_node_match(efi_device_path_protocol* a, efi_device_path_protocol* b) {
  size_t alen = a->Length[0] | (a->Length[1] << 8);
  size_t blen = b->Length[0] | (b->Length[1] << 8);
  if (alen != blen) {
    return false;
  }
  if (memcmp(a, b, alen)) {
    return false;
  }
  return true;
}

static efi_device_path_protocol* path_node_next(efi_device_path_protocol* node) {
  if (node->Type == DEVICE_PATH_END) {
    return NULL;
  }
  return ((void*)node) + (node->Length[0] | (node->Length[1] << 8));
}

static bool path_prefix_match(efi_device_path_protocol* path, efi_device_path_protocol* prefix) {
  if ((path == NULL) || (prefix == NULL)) {
    return false;
  }
  for (;;) {
    if (prefix->Type == DEVICE_PATH_END) {
      return true;
    }
    if (!path_node_match(path, prefix)) {
      return false;
    }
    if ((path = path_node_next(path)) == NULL) {
      return false;
    }
    prefix = path_node_next(prefix);
  }
}

static void print_path(efi_boot_services* bs, efi_device_path_protocol* path) {
  efi_device_path_to_text_protocol* ptt;
  efi_status status = bs->LocateProtocol(&DevicePathToTextProtocol, NULL, (void**)&ptt);
  if (status != EFI_SUCCESS) {
    printf("<cannot print path>");
    return;
  }
  char16_t* txt = ptt->ConvertDevicePathToText(path, false, false);
  if (txt == NULL) {
    printf("<cannot print path>");
    return;
  }
  puts16(txt);
  printf("\n");
  bs->FreePool(txt);
}

static efi_status disk_read(disk_t* disk, size_t offset, void* data, size_t length) {
  if (disk->first > disk->last) {
    return EFI_VOLUME_CORRUPTED;
  }

  uint64_t size = (disk->last - disk->first) * disk->blksz;
  if ((offset > size) || ((size - offset) < length)) {
    printf(
        "ERROR: Disk read invalid params. offset:%llu length:%llu disk: [%llu to %llu] size:%llu  "
        "blksz:%d\n",
        offset, size, length, disk->first, disk->last, disk->blksz);
    return EFI_INVALID_PARAMETER;
  }

  return disk->io->ReadDisk(disk->io, disk->id, (disk->first * disk->blksz) + offset, length, data);
}

efi_status disk_write(disk_t* disk, size_t offset, void* data, size_t length) {
  if (disk->first > disk->last) {
    return EFI_VOLUME_CORRUPTED;
  }

  uint64_t size = (disk->last - disk->first) * disk->blksz;
  if ((offset > size) || ((size - offset) < length)) {
    return EFI_INVALID_PARAMETER;
  }

  return disk->io->WriteDisk(disk->io, disk->id, (disk->first * disk->blksz) + offset, length,
                             data);
}

static void disk_close(disk_t* disk) {
  disk->bs->CloseProtocol(disk->h, &DiskIoProtocol, disk->img, NULL);
}

int disk_find_boot(efi_handle img, efi_system_table* sys, bool verbose, disk_t* disk) {
  bool found = false;
  efi_boot_services* bs = sys->BootServices;
  efi_handle* list;
  size_t count;
  efi_status status;
  efi_loaded_image_protocol* li;

  status = bs->OpenProtocol(img, &LoadedImageProtocol, (void**)&li, img, NULL,
                            EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (status != EFI_SUCCESS) {
    return -1;
  }

  efi_device_path_protocol* imgdevpath;
  status = bs->OpenProtocol(li->DeviceHandle, &DevicePathProtocol, (void**)&imgdevpath, img, NULL,
                            EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (status != EFI_SUCCESS) {
    goto fail_open_devpath;
  }

  if (verbose) {
    printf("BootLoader Path: ");
    print_path(bs, li->FilePath);
    printf("BootLoader Device: ");
    print_path(bs, imgdevpath);
  }

  status = bs->LocateHandleBuffer(ByProtocol, &BlockIoProtocol, NULL, &count, &list);
  if (status != EFI_SUCCESS) {
    printf("find_boot_disk() - no block io devices found\n");
    goto fail_get_list;
  }

  for (size_t n = 0; n < count; n++) {
    efi_block_io_protocol* bio;
    status = bs->OpenProtocol(list[n], &BlockIoProtocol, (void**)&bio, img, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (status != EFI_SUCCESS) {
      continue;
    }

    efi_device_path_protocol* path;
    status = bs->OpenProtocol(list[n], &DevicePathProtocol, (void**)&path, img, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (status != EFI_SUCCESS) {
      bs->CloseProtocol(list[n], &BlockIoProtocol, img, NULL);
      continue;
    }

    bool match = false;

    // if a non-logical partition, check for match
    if (!bio->Media->LogicalPartition && bio->Media->MediaPresent) {
      match = path_prefix_match(imgdevpath, path);
    }

    if (verbose) {
      printf("BlockIO Device: ");
      print_path(bs, path);
      printf("              : #%zu, %zuMB%s%s%s%s%s%s\n", n,
             bio->Media->LastBlock * bio->Media->BlockSize / 1024 / 1024,
             bio->Media->RemovableMedia ? " Removable" : "",
             bio->Media->MediaPresent ? " Present" : "",
             bio->Media->LogicalPartition ? " Logical" : "", bio->Media->ReadOnly ? " RO" : "",
             bio->Media->WriteCaching ? " WC" : "", match ? " BootDevice" : "");
    }

    if (match && !found) {
      status = bs->OpenProtocol(list[n], &DiskIoProtocol, (void**)&disk->io, img, NULL,
                                EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
      if (status != EFI_SUCCESS) {
        printf("find_boot_disk() - cannot get disk io protocol\n");
      } else {
        disk->first = 0;
        disk->last = bio->Media->LastBlock;
        disk->id = bio->Media->MediaId;
        disk->blksz = bio->Media->BlockSize;
        disk->h = list[n];
        disk->img = img;
        disk->bs = bs;
        found = true;
      }
    }

    bs->CloseProtocol(list[n], &BlockIoProtocol, img, NULL);
    bs->CloseProtocol(list[n], &DevicePathProtocol, img, NULL);
  }

  bs->FreePool(list);

fail_get_list:
  bs->CloseProtocol(li->DeviceHandle, &DevicePathProtocol, img, NULL);

fail_open_devpath:
  bs->CloseProtocol(img, &LoadedImageProtocol, img, NULL);

  return found ? 0 : -1;
}

int disk_find_partition(disk_t* disk, bool verbose, const uint8_t* guid_value,
                        const char* guid_name) {
  gpt_header_t gpt;
  efi_status status = disk_read(disk, disk->blksz, &gpt, sizeof(gpt));
  if (status != EFI_SUCCESS) {
    return -1;
  }

  if (gpt.magic != GPT_MAGIC) {
    printf("gpt - bad magic!\n");
    return -1;
  }

  if (verbose) {
    printf("gpt: size:    %u\n", gpt.size);
    printf("gpt: current: %zu\n", gpt.current);
    printf("gpt: backup:  %zu\n", gpt.backup);
    printf("gpt: first:   %zu\n", gpt.first);
    printf("gpt: last:    %zu\n", gpt.last);
    printf("gpt: entries: %zu\n", gpt.entries);
    printf("gpt: e.count: %u\n", gpt.entries_count);
    printf("gpt: e.size:  %u\n", gpt.entries_size);
  }

  if ((gpt.magic != GPT_MAGIC) || (gpt.size != GPT_HEADER_SIZE) ||
      (gpt.entries_size != GPT_ENTRY_SIZE) || (gpt.entries_count > 256)) {
    printf("gpt - malformed header\n");
    return -1;
  }

  gpt_entry_t* table;
  size_t tsize = gpt.entries_count * gpt.entries_size;

  status = disk->bs->AllocatePool(EfiLoaderData, tsize, (void**)&table);
  if (status != EFI_SUCCESS) {
    printf("gpt - allocation failure\n");
    return -1;
  }

  status = disk_read(disk, disk->blksz * gpt.entries, table, tsize);
  if (status != EFI_SUCCESS) {
    disk->bs->FreePool(table);
    printf("gpt - io error\n");
    return -1;
  }

  bool found = false;
  for (unsigned n = 0; n < gpt.entries_count; n++) {
    if ((table[n].first == 0) || (table[n].last == 0) || (table[n].last < table[n].first)) {
      // ignore empty or bogus entries
      continue;
    }

    const char* type;
    if (!memcmp(table[n].type, guid_value, GPT_GUID_LEN)) {
      type = guid_name;
      disk->first = table[n].first;
      disk->last = table[n].last;
      found = true;
    } else {
      type = "unknown";
    }

    if (verbose) {
      char name[GPT_NAME_LEN / 2];
      for (unsigned i = 0; i < GPT_NAME_LEN / 2; i++) {
        unsigned c = table[n].name[i * 2 + 0] | (table[n].name[i * 2 + 1] << 8);
        if ((c != 0) && ((c < ' ') || (c > 127))) {
          c = '.';
        }
        name[i] = c;
      }
      name[GPT_NAME_LEN / 2 - 1] = 0;
      printf("#%03d %zu..%zu %zx name='%s' type='%s'\n", n, table[n].first, table[n].last,
             table[n].flags, name, type);
    }
  }
  disk->bs->FreePool(table);

  return found ? 0 : -1;
}

void* image_load_from_disk(efi_handle img, efi_system_table* sys, size_t* _sz,
                           const uint8_t* guid_value, const char* guid_name) {
  static bool verbose = false;
  uint8_t sector[512];
  efi_boot_services* bs = sys->BootServices;
  disk_t disk;

  if (disk_find_boot(img, sys, verbose, &disk) < 0) {
    printf("Cannot find bootloader disk.\n");
    return NULL;
  }

  if (disk_find_partition(&disk, verbose, guid_value, guid_name)) {
    printf("Cannot find %s partition on bootloader disk.\n", guid_name);
    goto fail0;
  }

  efi_status status = disk_read(&disk, 0, sector, 512);
  if (status != EFI_SUCCESS) {
    printf("Failed to read disk: %zu\n", status);
    goto fail0;
  }

  size_t sz = image_getsize(sector, 512);
  if (sz == 0) {
    printf("%s partition has no valid header\n", guid_name);
    goto fail0;
  }

  size_t pages = (sz + 4095) / 4096;
  void* image;
  status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, (efi_physical_addr*)&image);
  if (status != EFI_SUCCESS) {
    printf("Failed to allocate %zu bytes to load %s image\n", sz, guid_name);
    goto fail0;
  }

  status = disk_read(&disk, 0, image, sz);
  if (status != EFI_SUCCESS) {
    printf("Failed to read image from %s partition\n", guid_name);
    goto fail1;
  }

  if (identify_image(image, sz) != IMAGE_COMBO) {
    printf("%s partition has no valid image\n", guid_name);
    goto fail1;
  }

  *_sz = sz;
  return image;

fail1:
  bs->FreePages((efi_physical_addr)image, pages);
fail0:
  disk_close(&disk);
  return NULL;
}

efi_status read_partition(efi_handle img, efi_system_table* sys, const uint8_t* guid_value,
                          const char* guid_name, uint64_t offset, unsigned char* data,
                          size_t size) {
  static bool verbose = false;
  disk_t disk;

  if (disk_find_boot(img, sys, verbose, &disk) < 0) {
    printf("Cannot find bootloader disk.\n");
    return EFI_NOT_FOUND;
  }

  if (disk_find_partition(&disk, verbose, guid_value, guid_name)) {
    printf("Cannot find %s partition on bootloader disk.\n", guid_name);
    disk_close(&disk);
    return EFI_NOT_FOUND;
  }

  efi_status status = disk_read(&disk, offset, data, size);
  disk_close(&disk);
  return status;
}

efi_status write_partition(efi_handle img, efi_system_table* sys, const uint8_t* guid_value,
                           const char* guid_name, uint64_t offset, const unsigned char* data,
                           size_t size) {
  static bool verbose = false;
  disk_t disk;

  if (disk_find_boot(img, sys, verbose, &disk) < 0) {
    printf("Cannot find bootloader disk.\n");
    return EFI_NOT_FOUND;
  }

  if (disk_find_partition(&disk, verbose, guid_value, guid_name)) {
    printf("Cannot find %s partition on bootloader disk.\n", guid_name);
    disk_close(&disk);
    return EFI_NOT_FOUND;
  }

  efi_status status = disk_write(&disk, offset, (void*)data, size);
  disk_close(&disk);
  return status;
}

int guid_value_from_name(char *guid_name, uint8_t *value) {
  if (!strncmp(guid_name, GUID_ZIRCON_A_NAME, GPT_NAME_LEN)) {
    uint8_t guid_value[GPT_GUID_LEN] = GUID_ZIRCON_A_VALUE;
    memcpy(value, guid_value, GPT_GUID_LEN);
  } else if (!strncmp(guid_name, GUID_ZIRCON_B_NAME, GPT_NAME_LEN)) {
    uint8_t guid_value[GPT_GUID_LEN] = GUID_ZIRCON_B_VALUE;
    memcpy(value, guid_value, GPT_GUID_LEN);
  } else if (!strncmp(guid_name, GUID_ZIRCON_R_NAME, GPT_NAME_LEN)) {
    uint8_t guid_value[GPT_GUID_LEN] = GUID_ZIRCON_R_VALUE;
    memcpy(value, guid_value, GPT_GUID_LEN);
  } else if (!strncmp(guid_name, GUID_VBMETA_A_NAME, GPT_NAME_LEN)) {
    uint8_t guid_value[GPT_GUID_LEN] = GUID_VBMETA_A_VALUE;
    memcpy(value, guid_value, GPT_GUID_LEN);
  } else if (!strncmp(guid_name, GUID_VBMETA_B_NAME, GPT_NAME_LEN)) {
    uint8_t guid_value[GPT_GUID_LEN] = GUID_VBMETA_B_VALUE;
    memcpy(value, guid_value, GPT_GUID_LEN);
  } else if (!strncmp(guid_name, GUID_VBMETA_R_NAME, GPT_NAME_LEN)) {
    uint8_t guid_value[GPT_GUID_LEN] = GUID_VBMETA_R_VALUE;
    memcpy(value, guid_value, GPT_GUID_LEN);
  } else if (!strncmp(guid_name, GUID_EFI_NAME, GPT_NAME_LEN)) {
    uint8_t guid_value[GPT_GUID_LEN] = GUID_EFI_VALUE;
    memcpy(value, guid_value, GPT_GUID_LEN);
  } else {
    return -1;
  }
  return 0;
}