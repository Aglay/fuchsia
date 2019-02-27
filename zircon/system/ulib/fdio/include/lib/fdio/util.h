// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FDIO_UTIL_H_
#define LIB_FDIO_UTIL_H_

#include <zircon/types.h>
#include <zircon/compiler.h>
#include <stdint.h>
#include <unistd.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/directory.h>

__BEGIN_CDECLS

// BEGIN DEPRECATED ------------------------------------------------------------
zx_status_t fdio_create_fd(zx_handle_t* handles, uint32_t* types, size_t hcount, int* fd_out);
// END DEPRECATED --------------------------------------------------------------

__END_CDECLS

#endif  // LIB_FDIO_UTIL_H_
