/*
 * Request Queue
 */

#ifndef _ReQ_H_
#define _ReQ_H_

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

struct ReQ{
	volatile atomic_flag _queue_lock; //Atomic lock of the RQ

	volatile uint32_t init_flag;

	volatile uint32_t qhead;
	volatile uint32_t qtail;
	volatile uint32_t qsize;
	volatile size_t ele_size; // Element size

	uint8_t* ring_buffer; //Point to the buffer

};

int ReQ_init(struct ReQ* req, uint8_t* buf, uint32_t length, size_t element_size);
int is_ReQ_init(struct ReQ* req);
void ReQ_append(struct ReQ* req, void* elem, size_t ele_size);
void ReQ_fetch(struct ReQ* req, void* elem, size_t ele_size);
int ReQ_fetch_batch(struct ReQ* req, void* elems, size_t ele_size);
void ReQ_print(struct ReQ* req);

#endif
