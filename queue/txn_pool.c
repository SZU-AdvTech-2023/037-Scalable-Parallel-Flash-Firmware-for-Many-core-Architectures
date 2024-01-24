#include "txn_pool.h"

#include "request_types.h"
#include "config.h"

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) & ((TYPE*)0)->MEMBER)
#endif

#define slot_entry(ptr)                     \
    ({                                                    \
        const typeof(((struct txn_slot*)0)->txn_body)* __mptr = (ptr); \
        (struct txn_slot*)((char*)__mptr - offsetof(struct txn_slot, txn_body));  \
    })


static struct txn_pool* txn_pool = (struct txn_pool*) TXN_POOL_BASE;

void init_txn_pool() {
	unsigned int i;
	struct txn_slot* txn_slot_ptr;

	txn_slot_ptr = txn_pool->txn_slot;
	for (i = 0; i < TXN_POOL_LENGTH; i++) {
		txn_slot_ptr->flag = NO_USE;
		txn_slot_ptr->txn_body.data = txn_pool->data_buffer + i * DMA_FLASH_PG_BUFFER_SIZE;
		txn_slot_ptr++;
	}
	txn_pool->alloc_index = 0;
}

flash_transaction* alloc_txn_slot() {
	unsigned int i, index;
	struct txn_slot* txn_slot_ptr;
	flash_transaction* txn_ptr;

	index = txn_pool->alloc_index;
	txn_slot_ptr = &(txn_pool->txn_slot[index]);
	for (i = 0; i < TXN_POOL_LENGTH; i++) {
		if (txn_slot_ptr->flag == NO_USE) break;
		index = (index + 1) % TXN_POOL_LENGTH;
		txn_slot_ptr = &(txn_pool->txn_slot[index]);
	}
	if (index == txn_pool->alloc_index && i > 0) {
		txn_ptr = NULL;
	} else {
		txn_ptr = &(txn_slot_ptr->txn_body);
		txn_slot_ptr->flag = IN_USE;
	}
	return txn_ptr;
}

void free_txn_slot(flash_transaction* txn) {
	struct txn_slot* txn_slot_ptr;

	txn_slot_ptr = slot_entry(txn);
	txn_slot_ptr->flag = NO_USE;
}
