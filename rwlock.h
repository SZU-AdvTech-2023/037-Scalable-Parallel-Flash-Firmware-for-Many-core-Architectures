/*
 * Read-Write lock
 */

#ifndef _RWLOCK_H_
#define _RWLOCK_H_

#include <stdint.h>
#include <stdatomic.h>


typedef struct RWlock{
    atomic_flag lock;
    uint32_t readers;
} rwlock;


void lock_w(rwlock* lock);
void unlock_w(rwlock* lock);
void lock_r(rwlock* lock);
void unlock_r(rwlock* lock);

#endif