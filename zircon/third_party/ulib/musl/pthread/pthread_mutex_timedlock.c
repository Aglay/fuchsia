#include "threads_impl.h"

int pthread_mutex_timedlock(pthread_mutex_t* restrict m, const struct timespec* restrict at) {
    if ((m->_m_type & PTHREAD_MUTEX_MASK) == PTHREAD_MUTEX_NORMAL &&
        !a_cas_shim(&m->_m_lock, 0, pthread_mutex_tid_to_uncontested_state(__thread_get_tid())))
        return 0;

    int r, t;

    r = pthread_mutex_trylock(m);
    if (r != EBUSY)
        return r;

    int spins = 100;
    while (spins-- && atomic_load(&m->_m_lock) && !atomic_load(&m->_m_waiters))
        a_spin();

    while ((r = pthread_mutex_trylock(m)) == EBUSY) {
        if (!(r = atomic_load(&m->_m_lock)))
            continue;
        if (((m->_m_type & PTHREAD_MUTEX_MASK) == PTHREAD_MUTEX_ERRORCHECK) &&
             (pthread_mutex_state_to_tid(r) == __thread_get_tid())) {
            return EDEADLK;
        }

        atomic_fetch_add(&m->_m_waiters, 1);
        t = pthread_mutex_uncontested_to_contested_state(r);
        a_cas_shim(&m->_m_lock, r, t);
        r = __timedwait(&m->_m_lock, t, CLOCK_REALTIME, at);
        atomic_fetch_sub(&m->_m_waiters, 1);
        if (r)
            break;
    }
    return r;
}
