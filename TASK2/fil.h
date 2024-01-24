#ifndef _FIL_FIL_H_
#define _FIL_FIL_H_

#include "list.h"
#include "request_types.h"

/* fil.c */
extern struct list_head finish_queue;
extern struct list_head wait_queue;

void fil_init(void);
void fil_dispatch(struct list_head* txn_list);
void fil_tick(void);
void parse_txn(flash_transaction* txn);
void scan_badblocks(); // only for fil function test

#endif
