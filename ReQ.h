/*
 * Request Queue
 */

#ifndef _ReQ_H_
#define _ReQ_H_

#include <stdint.h>
#include <stdatomic.h>


#define DATATYPE int

struct ReQ{
	atomic_flag _queue_lock; //Atomic lock of the RQ

	uint32_t init_flag;

	uint32_t qhead;
	uint32_t qtail;
	uint32_t qsize;

	DATATYPE* ring_buffer; //Point to the buffer
};

int ReQ_init(struct ReQ* req, char* buf, uint32_t size);
void ReQ_append(struct ReQ* req, uint32_t elem);
void ReQ_fetch(struct ReQ* req, uint32_t* elem);
void ReQ_print(struct ReQ* req);

#endif
