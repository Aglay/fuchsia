// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2008-2014 Travis Geiselbrecht
// Copyright (c) 2012 Shantanu Gupta
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <assert.h>
#include <debug.h>
#include <fbl/macros.h>
#include <fbl/canary.h>
#include <kernel/thread.h>
#include <kernel/lockdep.h>
#include <ktl/atomic.h>
#include <stdint.h>
#include <zircon/compiler.h>

// Kernel mutex support.
//
class __TA_CAPABILITY("mutex") Mutex {
public:
    constexpr Mutex()
        : wait_(WAIT_QUEUE_INITIAL_VALUE(this->wait_)) {}
    ~Mutex();

    // No moving or copying allowed.
    DISALLOW_COPY_ASSIGN_AND_MOVE(Mutex);

    void Acquire() __TA_ACQUIRE() __TA_EXCLUDES(thread_lock);
    void Release() __TA_RELEASE() __TA_EXCLUDES(thread_lock);

    // Special version of Release which operates with the thread lock held
    void ReleaseThreadLocked(bool reschedule) __TA_RELEASE() __TA_REQUIRES(thread_lock);

    // does the current thread hold the mutex?
    bool IsHeld() const {
        return (holder() == get_current_thread());
    }

private:
    enum class ThreadLockState : bool {
        NotHeld = false,
        Held = true
    };

    static constexpr uint32_t MAGIC = 0x6D757478;  // 'mutx'
    static constexpr uintptr_t FLAG_QUEUED = 1u;

    template <ThreadLockState TLS>
    void ReleaseInternal(bool reschedule) __TA_RELEASE() __TA_NO_THREAD_SAFETY_ANALYSIS;

    // Accessors to extract the holder pointer from the val member
    uintptr_t val() const {
        return val_.load(ktl::memory_order_relaxed);
    }

    static thread_t* holder_from_val(uintptr_t value) {
        return reinterpret_cast<thread_t*>(value & ~FLAG_QUEUED);
    }

    thread_t* holder() const {
        return holder_from_val(val());
    }

    fbl::Canary<MAGIC> magic_;
    ktl::atomic<uintptr_t> val_{0};
    wait_queue_t wait_;
};

// Lock policy for kernel mutexes
//
struct MutexPolicy {
    // No extra state required for mutexes.
    struct State {};

    // Basic acquire and release operations.
    template <typename LockType>
    static bool Acquire(LockType* lock, State*) __TA_ACQUIRE(lock) __TA_EXCLUDES(thread_lock) {
        lock->Acquire();
        return true;
    }
    template <typename LockType>
    static void Release(LockType* lock, State*) __TA_RELEASE(lock) __TA_EXCLUDES(thread_lock) {
        lock->Release();
    }

    // A enum tag that can be passed to Guard<Mutex>::Release(...) to
    // select the special-case release method below.
    enum SelectThreadLockHeld { ThreadLockHeld };

    // Specifies whether the special-case release method below should
    // reschedule.
    enum RescheduleOption : bool {
        NoReschedule = false,
        Reschedule = true,
    };

    // Releases the lock using the special mutex release operation. This
    // is selected by calling:
    //
    //  Guard<TrivialMutex|Mutex|Mutex>::Release(ThreadLockHeld [, Reschedule | NoReschedule])
    //
    template <typename LockType>
    static void Release(LockType* lock,
                        State*,
                        SelectThreadLockHeld,
                        RescheduleOption reschedule = Reschedule)
    __TA_RELEASE(lock) __TA_REQUIRES(thread_lock) {
        lock->ReleaseThreadLocked(reschedule);
    }
};

// Configure the lockdep::Guard for kernel mutexes to use MutexPolicy.
LOCK_DEP_POLICY(Mutex, MutexPolicy);

// Declares a Mutex member of the struct or class |containing_type|.
//
// Example usage:
//
// struct MyType {
//     DECLARE_MUTEX(MyType) lock;
// };
//
#define DECLARE_MUTEX(containing_type) \
    LOCK_DEP_INSTRUMENT(containing_type, ::Mutex)

// Declares a |lock_type| member of the struct or class |containing_type|.
//
// Example usage:
//
// struct MyType {
//     DECLARE_LOCK(MyType, LockType) lock;
// };
//
#define DECLARE_LOCK(containing_type, lock_type) \
    LOCK_DEP_INSTRUMENT(containing_type, lock_type)

// By default, singleton mutexes in the kernel use Mutex in order to avoid
// a useless global dtor
//
// Example usage:
//
//  DECLARE_SINGLETON_MUTEX(MyGlobalLock [, LockFlags]);
//
#define DECLARE_SINGLETON_MUTEX(name, ...) \
    LOCK_DEP_SINGLETON_LOCK(name, ::Mutex, ##__VA_ARGS__)

// Declares a singleton |lock_type| with the name |name|.
//
// Example usage:
//
//  DECLARE_SINGLETON_LOCK(MyGlobalLock, LockType, [, LockFlags]);
//
#define DECLARE_SINGLETON_LOCK(name, lock_type, ...) \
    LOCK_DEP_SINGLETON_LOCK(name, lock_type, ##__VA_ARGS__)
