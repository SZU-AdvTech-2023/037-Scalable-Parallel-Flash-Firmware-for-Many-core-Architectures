#ifndef _TxnP_H_
#define _TxnP_H_

#include "config.h"
#include "request_types.h"
#include "utils.h"

#define IN_USE 1
#define NO_USE 0

#define DMA_FLASH_PG_BUFFER_SIZE ALIGNMENT_UP(FLASH_PG_BUFFER_SIZE, 32)

struct txn_slot {
	volatile int flag; // describe the validate
	flash_transaction txn_body;
};

struct txn_pool {
	unsigned char data_buffer[DMA_FLASH_PG_BUFFER_SIZE * TXN_POOL_LENGTH];
	struct txn_slot txn_slot[TXN_POOL_LENGTH];
	unsigned int alloc_index;
};

void init_txn_pool();
flash_transaction* alloc_txn_slot();
void free_txn_slot(flash_transaction* txn);
#endif
