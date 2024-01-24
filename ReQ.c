#include <stdio.h>
#include <stdbool.h>
#include "ReQ.h"
#include "config.h"


int ReQ_init(struct ReQ* req, char* buf, uint32_t size){
	//TODO: Test the init function after mapping a piece of memory.
	atomic_flag_clear(&(req->_queue_lock));
	req->qhead = 0;
	req->qtail = 0;
	req->qsize = size;
	req->ring_buffer = (DATATYPE*) buf;
	req->init_flag = 1;
	return 0;
}

static bool _is_full(struct ReQ* req){
	return req->qtail == (req->qhead + req->qsize) ? true : false;
}

static bool _is_empty(struct ReQ* req){
	return req->qtail == req->qhead ? true : false;
}

void ReQ_append(struct ReQ* req, uint32_t elem){
	while(atomic_flag_test_and_set(&(req->_queue_lock))); //Acquire the spin lock
	while(_is_full(req)){
		atomic_flag_clear(&(req->_queue_lock)); //If the queue is full, then release the lock.
#ifdef DEBUG
		printf("[ReQ] The ring_buf is full.\n");
#endif
		while(atomic_flag_test_and_set(&(req->_queue_lock)));
	}
	uint32_t index = req->qtail % req->qsize;
	req->ring_buffer[index] = elem;
	req->qtail++;
	atomic_flag_clear(&(req->_queue_lock)); //Release the lock
}

void ReQ_fetch(struct ReQ* req, uint32_t* elem){
	while(atomic_flag_test_and_set(&(req->_queue_lock))); //Acquire the spin lock
	while(_is_empty(req)){
		atomic_flag_clear(&(req->_queue_lock)); //If the queue is empty, then release the lock.
#ifdef DEBUG
		printf("[ReQ] The ring_buf is empty.\n");
#endif
		while(atomic_flag_test_and_set(&(req->_queue_lock)));
	}
	uint32_t index = req->qhead % req->qsize;
	*elem = req->ring_buffer[index];
	req->qhead++;
	atomic_flag_clear(&(req->_queue_lock)); //Release the lock
}

static void _reset_index(struct ReQ* req){
	uint32_t elemnum = req->qtail - req->qhead;
	req->qhead %= req->qsize;
	req->qtail = req->qhead + elemnum;
}

void ReQ_print(struct ReQ* req){
    printf("atomic_flag: %d\n", req->_queue_lock);
    printf("init_flag: %d\n", req->init_flag);
    printf("qhead: %d\n", req->qhead);
    printf("qtail: %d\n", req->qtail);
    printf("qsize: %d\n", req->qsize);
    printf("ring_buffer: %u\n", req->ring_buffer);
}