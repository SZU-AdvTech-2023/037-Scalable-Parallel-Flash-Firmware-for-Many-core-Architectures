#include <stdbool.h>
#include "rwlock.h"

static int spin_lock(atomic_flag* lock){
    while(atomic_flag_test_and_set(lock));
    return 0;
}

static int spin_unlock(atomic_flag* lock){
    atomic_flag_clear(lock);
    return 0;
}

void lock_w(rwlock* lock){
    spin_lock(&lock->lock);
    while(lock->readers);
}

void unlock_w(rwlock* lock){
    spin_unlock(&lock->lock);
}

void lock_r(rwlock* lock){
    atomic_fetch_add(&lock->readers, 1);
    if(!(lock->lock.__val)) return 0;
    atomic_fetch_sub(&lock->readers, 1);
}

void unlock_r(rwlock* lock){
    atomic_fetch_sub(&lock->readers, 1);
}