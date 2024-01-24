#include "request_types.h"
#include "req_queue.h"
#include "config.h"
#include "fil.h"

struct ReQ* in_queue = (struct ReQ*)REQ_2_QUEUE_META_BASE;
struct ReQ* out_queue = (struct ReQ*)REQ_3_QUEUE_META_BASE;


int main(){
	flash_transaction *txns[1024];
	flash_transaction *this_txn, *tmp;
	int req_num;

	ReQ_init(in_queue, (u8*)REQ_1_QUEUE_DATA_BASE, 128, sizeof(void*));
	fil_init();
	while (is_ReQ_init(in_queue) != 1);

	while (1) {

		req_num = ReQ_fetch_batch(in_queue, (void*)txns, sizeof(void*));
		dispatch_requests_taskQ(txns, req_num);
		dispatch_requests_waitQ();
		list_for_each_entry_safe(this_txn, tmp, &finish_queue, txn_list_handle){
			list_del(&this_txn->txn_list_handle);
			ReQ_append(out_queue, (void*)&this_txn, sizeof(void*));
		}

	}
}
