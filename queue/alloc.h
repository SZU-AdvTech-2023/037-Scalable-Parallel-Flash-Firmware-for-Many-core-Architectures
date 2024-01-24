#ifndef _ALLOC_H_
#define _ALLOC_H_

void mem_init(int mem_zone, unsigned long mem_start, size_t free_mem_size);
void* alloc_mem(size_t memsize, size_t alignment, int flags);
void* alloc_pages(size_t nr_pages, int flags);
void free_mem(void* base, unsigned long len);

#endif
