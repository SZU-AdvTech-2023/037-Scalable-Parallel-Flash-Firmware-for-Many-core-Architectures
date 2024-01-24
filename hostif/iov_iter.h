#ifndef _IOV_ITER_H_
#define _IOV_ITER_H_

#include <stddef.h>

#define FALSE 0
#define TRUE 1

typedef struct {
    void* iov_base;
    size_t iov_len;
} iovec;

typedef struct {
    size_t iov_offset;
    size_t count;
    const iovec* iov;
    size_t nr_segs;
} iov_iter;

void iov_iter_init(iov_iter* iter, const iovec* iov,
                   size_t nr_segs, size_t count);

size_t iov_iter_copy_from(iov_iter* iter, void* buf, size_t bytes);
size_t iov_iter_copy_to(iov_iter* iter, const void* buf, size_t bytes);

int iov_iter_get_bufaddr(iov_iter* iter, void** buf, size_t* bytes);
void iov_iter_consume(iov_iter* iter, size_t bytes);

#endif
