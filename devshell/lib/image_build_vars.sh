# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/vars.sh
fx-config-read

zircon_bin="zircon.bin"
ramdisk_bin="bootdata-blobstore-${ZIRCON_PROJECT}.bin"

images_dir="images"
cmdline_txt="${images_dir}/cmdline.txt"
efi_block="${images_dir}/efi.blk"
fvm_block="${images_dir}/fvm.blk"
fvm_sparse_block="${images_dir}/fvm.sparse.blk"
kernc_vboot="${images_dir}/kernc.vboot"