// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_SPINLOCK_H_
#define ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_SPINLOCK_H_

#include <lib/zircon-internal/thread_annotations.h>
#include <stdbool.h>
#include <zircon/compiler.h>

#include <arch/x86.h>
#include <arch/x86/mp.h>
#include <kernel/atomic.h>
#include <kernel/cpu.h>

__BEGIN_CDECLS

#define ARCH_SPIN_LOCK_INITIAL_VALUE \
  (arch_spin_lock_t) { 0 }

typedef struct TA_CAP("mutex") arch_spin_lock {
  unsigned long value;
} arch_spin_lock_t;

typedef x86_flags_t spin_lock_saved_state_t;

void arch_spin_lock(arch_spin_lock_t *lock) TA_ACQ(lock);
bool arch_spin_trylock(arch_spin_lock_t *lock) TA_TRY_ACQ(false, lock);
void arch_spin_unlock(arch_spin_lock_t *lock) TA_REL(lock);

static inline cpu_num_t arch_spin_lock_holder_cpu(const arch_spin_lock_t *lock) {
  return (cpu_num_t)__atomic_load_n(&lock->value, __ATOMIC_RELAXED) - 1;
}

static inline bool arch_spin_lock_held(arch_spin_lock_t *lock) {
  return arch_spin_lock_holder_cpu(lock) == arch_curr_cpu_num();
}

static inline void arch_interrupt_save(spin_lock_saved_state_t *statep) {
  *statep = x86_save_flags();
  x86_cli();
  atomic_signal_fence();
}

static inline void arch_interrupt_restore(spin_lock_saved_state_t old_state) {
  x86_restore_flags(old_state);
}

__END_CDECLS

#endif  // ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_SPINLOCK_H_
