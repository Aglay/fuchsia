// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zircon/types.h>

#include <fbl/intrusive_wavl_tree.h>
#include <ffl/fixed.h>

#include <utility>

// Forward declaration.
typedef struct thread thread_t;

// Fixed-point task weight/priority. The 5bit fractional component supports 32
// priority levels (1/32 through 32/32), while the 26bit integer component
// supports sums of ~64M threads with weight 1.0.
//
// Weights should not be negative however, the value is signed for consistency
// with zx_time_t (SchedTime) and zx_duration_t (SchedDuration), which are the
// primary types used in conjunction with SchedWeight. This is to make it less
// likely that expressions involving weights are accidentally promoted to
// unsigned.
using SchedWeight = ffl::Fixed<int32_t, 5>;

// Fixed-point types wrapping time and duration types to make time expressions
// cleaner in the scheduler code.
using SchedDuration = ffl::Fixed<zx_duration_t, 0>;
using SchedTime = ffl::Fixed<zx_time_t, 0>;

// Utilities that return fixed-point Expression representing the given integer
// time units in terms of system time units (nanoseconds).
template <typename T>
constexpr auto SchedNs(T nanoseconds) {
    return ffl::FromInteger(ZX_NSEC(nanoseconds));
}
template <typename T>
constexpr auto SchedUs(T microseconds) {
    return ffl::FromInteger(ZX_USEC(microseconds));
}
template <typename T>
constexpr auto SchedMs(T milliseconds) {
    return ffl::FromInteger(ZX_MSEC(milliseconds));
}

// Per-thread state used by FairScheduler.
class FairTaskState {
public:
    // The key type of this node operated on by WAVLTree.
    using KeyType = std::pair<SchedTime, uint64_t>;

    FairTaskState() = default;

    explicit FairTaskState(SchedWeight weight)
        : base_weight_{weight} {}

    FairTaskState(const FairTaskState&) = delete;
    FairTaskState& operator=(const FairTaskState&) = delete;

    // TODO(eieio): Implement inheritance.
    SchedWeight base_weight() const { return base_weight_; }
    SchedWeight effective_weight() const { return base_weight_; }

    // Returns the key used to order the run queue.
    KeyType key() const { return {virtual_finish_time_, generation_}; }

    // Returns true of the task state is currently enqueued in the runnable tree.
    bool InQueue() const {
        return run_queue_node_.InContainer();
    }

    // Returns true if the task is active (queued or running) on a run queue.
    bool active() const { return active_; }

    // Sets the task state to active (on a run queue). Returns true if the task
    // was not previously active.
    bool OnInsert() {
        const bool was_active = active_;
        active_ = true;
        return !was_active;
    }

    // Sets the task state to inactive (not on a run queue). Returns true if the
    // task was previously active.
    bool OnRemove() {
        const bool was_active = active_;
        active_ = false;
        return was_active;
    }

    // Returns the generation count from the last time the thread was enqueued
    // in the runnable tree.
    uint64_t generation() const { return generation_; }

private:
    friend class FairScheduler;

    // WAVLTree node state.
    fbl::WAVLTreeNodeState<thread_t*> run_queue_node_;

    // The base weight of the thread.
    SchedWeight base_weight_{0};

    // Flag indicating whether this thread is associated with a runqueue.
    bool active_{false};

    // TODO(eieio): Some of the values below are only relevant when running,
    // while others only while ready. Consider using a union to save space.

    // The virtual time of the thread's current bandwidth request.
    SchedTime virtual_start_time_{0};

    // The virtual finish time of the thread's current bandwidth request.
    SchedTime virtual_finish_time_{0};

    // The current timeslice allocated to the thread.
    SchedDuration time_slice_ns_{0};

    // The remainder of timeslice allocated to the thread when it blocked.
    SchedDuration lag_time_ns_{0};

    // Takes the value of FairScheduler::generation_count_ + 1 at the time this
    // node is added to the run queue.
    uint64_t generation_{0};
};
