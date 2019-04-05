// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <kernel/mutex.h>
#include <object/channel_dispatcher.h>
#include <object/excp_port.h>
#include <zircon/thread_annotations.h>
#include <zircon/types.h>

#include <fbl/ref_ptr.h>

class ExceptionDispatcher;

// Kernel-owned exception channel endpoint.
//
// This class is thread-safe, does not require external synchronization.
class Exceptionate {
public:
    // Jobs and processes need to distinguish between standard or debug
    // exception handlers.
    enum class Type {
        kStandard,
        kDebug
    };

    // Once ports are removed we can switch to using the userspace exception
    // constants directly, but for now this keeps the code cleaner.
    Exceptionate(ExceptionPort::Type port_type);

    ExceptionPort::Type port_type() const { return port_type_; }

    // Sets the backing ChannelDispatcher endpoint.
    //
    // The exception channel is first-come-first-served, so if there is
    // already a valid channel in place (i.e. has a live peer) this will
    // fail.
    //
    // Returns:
    //   ZX_ERR_INVALID_ARGS if |channel| is null.
    //   ZX_ERR_ALREADY_BOUND is there is already a valid channel.
    //   ZX_ERR_BAD_STATE if Shutdown() has already been called.
    zx_status_t SetChannel(fbl::RefPtr<ChannelDispatcher> channel);

    // Removes any exception channel, which will signal PEER_CLOSED for the
    // userspace endpoint.
    //
    // Any further attempt to set a new channel will return ZX_ERR_BAD_STATE.
    void Shutdown();

    // Sends an exception to userspace.
    //
    // The exception message contains:
    //  * 1 struct: zx_exception_info_t
    //  * 1 handle: ExceptionDispatcher
    //
    // Returns:
    //   ZX_ERR_NEXT if there is no valid underlying channel.
    //   ZX_ERR_NO_MEMORY if we failed to allocate memory.
    zx_status_t SendException(fbl::RefPtr<ExceptionDispatcher> exception);

private:
    const ExceptionPort::Type port_type_;
    mutable DECLARE_MUTEX(Exceptionate) lock_;
    fbl::RefPtr<ChannelDispatcher> channel_ TA_GUARDED(lock_);
    bool is_shutdown_ TA_GUARDED(lock_) = false;
};
