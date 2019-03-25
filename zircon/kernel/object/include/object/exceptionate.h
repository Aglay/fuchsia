// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <kernel/mutex.h>
#include <object/channel_dispatcher.h>
#include <zircon/thread_annotations.h>
#include <zircon/types.h>

#include <fbl/ref_ptr.h>

// Kernel-owned exception channel endpoint.
//
// This class is thread-safe, does not require external synchronization.
class Exceptionate {
public:
    Exceptionate() = default;

    // Sets the backing ChannelDispatcher endpoint.
    //
    // The exception channel is first-come-first-served, so if there is
    // already a valid channel in place (i.e. has a live peer) this will
    // fail.
    //
    // Returns:
    //   ZX_ERR_INVALID_ARGS if |channel| is null.
    //   ZX_ERR_ALREADY_BOUND is there is already a valid channel.
    zx_status_t SetChannel(fbl::RefPtr<ChannelDispatcher> channel);

private:
    mutable DECLARE_MUTEX(Exceptionate) lock_;
    fbl::RefPtr<ChannelDispatcher> channel_ TA_GUARDED(lock_);
};
