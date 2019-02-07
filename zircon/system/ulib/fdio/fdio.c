// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lib/fdio/io.h>
#include <lib/zxio/null.h>

#include "private.h"

struct fdio {
    fdio_ops_t* ops;
    atomic_int_fast32_t refcount;
    int32_t dupcount;
    uint32_t ioflag;
    zxio_storage_t storage;
};

// fdio_reserved_io is a globally shared fdio_t that is used to represent a
// reservation in the fdtab. If a user observes fdio_reserved_io there is a race
// condition in their code or they are looking up fd's by number.
// fdio_reserved_io is used in the time between a user requesting an operation
// that creates and fd, and the time when a remote operation to create the
// backing fdio_t is created, without holding the fdtab lock. Examples include
// open() of a file, or accept() on a socket.
static fdio_t fdio_reserved_io = {
    // TODO(raggi): It may be ideal to replace these operations with ones that
    // more directly encode the result that a user must have implemented a race
    // in order to invoke them.
    .ops = NULL,
    .refcount = 1,
    .dupcount = 1,
    .ioflag = 0,
};

fdio_t* fdio_get_reserved_io(void) {
    return &fdio_reserved_io;
}

zxio_t* fdio_get_zxio(fdio_t* io) {
    return &io->storage.io;
}

const fdio_ops_t* fdio_get_ops(const fdio_t* io) {
    return io->ops;
}

int32_t fdio_get_dupcount(const fdio_t* io) {
    return io->dupcount;
}

void fdio_dupcount_acquire(fdio_t* io) {
    io->dupcount++;
}

void fdio_dupcount_release(fdio_t* io) {
    io->dupcount--;
}

uint32_t* fdio_get_ioflag(fdio_t* io) {
    return &io->ioflag;
}

zxio_storage_t* fdio_get_zxio_storage(fdio_t* io) {
    return &io->storage;
}

fdio_t* fdio_alloc(fdio_ops_t* ops) {
    fdio_t* io = (fdio_t*) calloc(1, sizeof(fdio_t));
    LOG(5, "fdio: io: alloc: %p\n", io);
    io->ops = ops;
    atomic_init(&io->refcount, 1);
    return io;
}

void fdio_acquire(fdio_t* io) {
    LOG(6, "fdio: acquire: %p\n", io);
    atomic_fetch_add(&io->refcount, 1);
}

void fdio_release(fdio_t* io) {
    LOG(6, "fdio: release: %p\n", io);
    if (atomic_fetch_sub(&io->refcount, 1) == 1) {
        LOG(5, "fdio: io: free: %p\n", io);
        io->ops = NULL;
        free(io);
    }
}

bool fdio_is_last_reference(fdio_t* io) {
    return atomic_load(&io->refcount) == 1;
}
