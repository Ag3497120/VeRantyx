// Spin locks in memory shared between vxrt processes (named objects' state and their holder registries, the handle
// inboxes of DuplicateHandle, the shared texture table): the lock word holds the pid of the process that has it.
//
// On Windows the kernel keeps these objects, so no program can leave one locked by ending. Here a process ends at
// once - TerminateProcess on itself is _exit while its other threads run on, TerminateProcess from another process is
// SIGKILL, a crash ends it too - and a thread of it may have been between lock and unlock. With a plain 0/1 word the
// lock then stayed taken for good: POSIX shared memory outlives processes, so every later vxrt process that touched
// the name spun forever (observed 2026-10-25: Steam's web helper ended itself with TerminateProcess while a thread
// held the registry of "GameOverlayRender_DetourErrorStream_mutex-IPCWrapper"; every web helper started after it hung
// in CreateMutexA at 100 % CPU before Chromium started, until the memory was gone). Now a waiter that finds the lock
// held by a process that no longer exists takes it over, as Windows hands an abandoned mutex to the next waiter. What
// the ended process was changing may be half done; each structure guarded this way stays usable with that (a holder
// list that is checked against live processes, counts and flags of an object).
//
// A word of 1 was written by vxrt builds before this one, which did not record the holder: such a lock is taken over
// once it has stayed held for two seconds (these locks guard a few stores; no live holder keeps one that long).
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vx.h"

static int process_gone(uint32_t pid) { return pid > 1 && kill((pid_t)pid, 0) && errno == ESRCH; }

void vx_shlock(volatile uint32_t *word) {
    uint32_t me = (uint32_t)getpid(), cur;
    uint64_t legacy_since = 0;
    for (unsigned n = 0;; n++) {
        cur = 0;
        if (__atomic_compare_exchange_n(word, &cur, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if ((n & 255) == 255 && cur != me) {                       // (another thread of this process: it is running)
            int take = 0;
            if (cur == 1) {
                uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                if (!legacy_since) legacy_since = now;
                else take = now - legacy_since > 2000000000ull;
            } else {
                legacy_since = 0;
                take = process_gone(cur);
            }
            if (take && __atomic_compare_exchange_n(word, &cur, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                vx_log("shared lock %p: taken over from %s %u, which ended holding it", (void *)word,
                       cur == 1 ? "an older vxrt process (not recorded), held 2 s; process" : "process", cur);
                return;
            }
        }
        sched_yield();
    }
}

void vx_shunlock(volatile uint32_t *word) { __atomic_store_n(word, 0, __ATOMIC_RELEASE); }

// VXRT_TEST_SHLOCK_DIE=<object name>: a process creating or opening that named event, mutex or semaphore ends
// (_exit(3)) right after taking the lock of the name's holder registry, still holding it (src/sync.c), so that
// tests/shlock.c can show the next process taking it over. Off otherwise.
int vx_shlock_test_name(const char *name) {
    static const char *die;
    if (!die) { die = getenv("VXRT_TEST_SHLOCK_DIE"); if (!die) die = ""; }
    return *die && name && !strcmp(die, name);
}
