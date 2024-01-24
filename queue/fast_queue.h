/*
 * FastForward PPoPP08
 */

#ifndef _ReQ_H_
#define _ReQ_H_

#define BUFFER_SIZE	2048

struct FastQueue {
	void* buffer[BUFFER_SIZE];
	volatile void* head;
	volatile void* tail;
};

void init_queue(struct FastQueue* q);
int enqueue_nonblock(struct FastQueue* q, void* data);
int dequeue_nonblock(struct FastQueue* q, void* data);

#endif
