// Exercise the actual address-keyed pthread shim, without a guest or GPU.
#include "klepton.h"
#include "kl_glibc.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { count = 4096, slots = 65536, stride = 64 };
static void *mutexes[count];
static int (*guest_init)(void *, const void *);
static int (*guest_lock)(void *), (*guest_unlock)(void *);
static int (*guest_trylock)(void *), (*guest_destroy)(void *);

static void *check_held(void *unused) {
    (void)unused;
    unsigned split = 0;
    for (unsigned i = 1; i < count; i += 2) {
        int rc = guest_trylock(mutexes[i]);
        if (rc == 0) { split++; assert(guest_unlock(mutexes[i]) == 0); }
        else assert(rc == 16); // Linux EBUSY
    }
    printf("Guest mutexes admitting a second owner after unrelated destruction: %u\n", split);
    fflush(stdout);
    assert(split == 0);
    return NULL;
}

int main(void) {
    kl_glibc_install("synthetic-mutex-test");
    guest_init = kl_shim_lookup("pthread_mutex_init");
    guest_lock = kl_shim_lookup("pthread_mutex_lock");
    guest_unlock = kl_shim_lookup("pthread_mutex_unlock");
    guest_trylock = kl_shim_lookup("pthread_mutex_trylock");
    guest_destroy = kl_shim_lookup("pthread_mutex_destroy");
    assert(guest_init && guest_lock && guest_unlock && guest_trylock && guest_destroy);
    char *storage = calloc(slots, stride); assert(storage);
    unsigned *order = malloc(slots * sizeof(*order)); assert(order);
    for (unsigned i = 0; i < slots; i++) order[i] = i;
    uint32_t random = 0x9e3779b9;
    // Spread valid guest objects across a pool, producing hash collisions
    // without depending on the map's hash or exposing its private entries.
    for (unsigned i = 0; i < count; i++) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        unsigned pick = i + random % (slots-i), slot = order[pick];
        order[pick] = order[i]; order[i] = slot;
        mutexes[i] = storage + slot * stride;
        assert(guest_init(mutexes[i], NULL) == 0);
        if (i & 1) assert(guest_lock(mutexes[i]) == 0);
    }
    for (unsigned i = 0; i < count; i += 2) assert(guest_destroy(mutexes[i]) == 0);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, check_held, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    for (unsigned i = 1; i < count; i += 2) {
        assert(guest_unlock(mutexes[i]) == 0);
        assert(guest_destroy(mutexes[i]) == 0);
    }
    free(storage);
    free(order);
    puts("Native host: unrelated mutex destruction preserves mutual exclusion");
    return 0;
}
