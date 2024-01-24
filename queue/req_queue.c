#include "req_queue.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "config.h"


int ReQ_init(struct ReQ* req, uint8_t* buf, uint32_t length, size_t element_size){
	//TODO: Test the init function after mapping a piece of memory.
	atomic_flag_clear(&(req->_queue_lock));
	req->qhead = 0;
	req->qtail = 0;
	req->qsize = length;
	req->ring_buffer = buf;
	req->init_flag = 1;
	req->ele_size = element_size;
	return 0;
}

int is_ReQ_init(struct ReQ* req) {
	return req->init_flag == 1 ? 1 : 0;
}

static bool _is_full(struct ReQ* req){
	return req->qtail == (req->qhead + req->qsize) ? true : false;
}

static bool _is_empty(struct ReQ* req){
	return req->qtail == req->qhead ? true : false;
}

void ReQ_append(struct ReQ* req, void* elem, size_t ele_size){
	while(atomic_flag_test_and_set(&(req->_queue_lock))); //Acquire the spin lock
	while(_is_full(req)){
		atomic_flag_clear(&(req->_queue_lock)); //If the queue is full, then release the lock.
#ifdef DEBUG
		printf("[ReQ] The ring_buf is full.\n");
#endif
		while(atomic_flag_test_and_set(&(req->_queue_lock)));
	}
	unsigned int index = req->qtail % req->qsize;
	void* data_location = (void*)(req->ring_buffer + index * req->ele_size);
	memcpy(data_location, elem, ele_size);
	req->qtail++;
	atomic_flag_clear(&(req->_queue_lock)); //Release the lock
}

void ReQ_fetch(struct ReQ* req, void* elem, size_t  ele_size){
	while(atomic_flag_test_and_set(&(req->_queue_lock))); //Acquire the spin lock
	while(_is_empty(req)){
		atomic_flag_clear(&(req->_queue_lock)); //If the queue is empty, then release the lock.
#ifdef DEBUG
		printf("[ReQ] The ring_buf is empty.\n");
#endif
		while(atomic_flag_test_and_set(&(req->_queue_lock)));
	}
	unsigned int index = req->qhead % req->qsize;
	void* data_location = (void*)(req->ring_buffer + index * req->ele_size);
	memcpy(elem, data_location, ele_size);
	req->qhead++;
	atomic_flag_clear(&(req->_queue_lock)); //Release the lock
}

int ReQ_fetch_batch(struct ReQ* req, void* elems, size_t  ele_size){
	while(atomic_flag_test_and_set(&(req->_queue_lock))); //Acquire the spin lock
	while(_is_empty(req)){
		atomic_flag_clear(&(req->_queue_lock)); //If the queue is empty, then release the lock.
#ifdef DEBUG
		printf("[ReQ] The ring_buf is empty.\n");
#endif
		while(atomic_flag_test_and_set(&(req->_queue_lock)));
	}
	int fetched_num = req->qtail - req->qhead;
	unsigned int index = req->qhead % req->qsize;
	void* data_location = (void*)(req->ring_buffer + index * req->ele_size);
	memcpy(elems, data_location, ele_size * fetched_num);
	req->qhead = req->qtail;
	atomic_flag_clear(&(req->_queue_lock)); //Release the lock
	return fetched_num;
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
