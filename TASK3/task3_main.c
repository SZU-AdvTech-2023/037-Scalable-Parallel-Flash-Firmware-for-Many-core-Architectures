#include <stdlib.h>
#include <string.h>

#include "hostif.h"
#include "nvme.h"
#include "txn_pool.h"
#include "cache.h"
#include "config.h"
#include "iov_iter.h"
#include "pcie_soft_intf.h"

typedef struct {
    struct list_head list;
	int req_id;
	int txn_nums;
} LinkedList;

LinkedList txn_list[33];

#define WORKLIST_NUM 32
#define wait_index 32
#define NO_reqId -1
#define NOforFree -1

static void insert_waitlist(int index);
static void insert_to_list(int index,flash_transaction* txn);

datacache* DCache = (datacache*)DCache_BASE;

static void update_cache(flash_transaction* txn, unsigned int line){
	// GetWLock(DCache, line); // Get Write Lock
	SetData(DCache, line, txn->data);
	SetTag(DCache, line, txn->lpa);//first setdata and then set tag
	// ReleaseWLock(DCache, line); // Release Write Lock
}


static void wrap_admin_txn(flash_transaction* txn){
	post_cqe(&nvme_queues[0], txn->Status, txn->req_id, &txn->result_adm);
	pcie_send_msi(nvme_queues[0].cq_vector);
}

static void handle_read(struct list_head* txn_list, uint64_t prp1, uint64_t prp2){
	iovec iov[1 << MAX_DATA_TRANSFER_SIZE];
	iovec* iov_ptr;
	iov_iter iter;
	size_t count = 0;
	int iov_index = 0;

	flash_transaction *this_txn,*tmp;

	iov_ptr = iov;
	list_for_each_entry_safe(this_txn, tmp, txn_list, txn_list_handle){
		iov_ptr->iov_base = this_txn->data + this_txn->offset;
		iov_ptr->iov_len = this_txn->length;

		iov_ptr++;
		iov_index++;
		count += this_txn->length;
	}
	iov_iter_init(&iter, iov, iov_index, count);
	write_prp_data(&iter, prp1 , prp2 , count);
}

static void insert_waitlist(int index){
	flash_transaction *this_txn,*tmp;
	int req_id = 0;
	int first = TRUE;

	list_for_each_entry_safe(this_txn, tmp, &(txn_list[wait_index].list), txn_list_handle){
		if(first){
			// put the first txn message to the new free list
			txn_list[index].req_id = this_txn->req_id;
			req_id = this_txn->req_id;
			first = FALSE;
		}
		// if found the same req_id,then delete from wait list and insert to new free list
		if(this_txn->req_id == req_id){
			list_del(&this_txn->txn_list_handle);
			insert_to_list(index, this_txn);
		}
	}
}

static void insert_to_list(int index, flash_transaction* txn){
	flash_transaction *this_txn, *tmp;
	struct list_head *pre = &txn_list[index].list;
<<<<<<< HEAD
	unsigned int cacheline;

=======
	lpa_t lpa = txn->lpa;
	unsigned int cacheline = lpa % CACHE_LINE_NUM;
>>>>>>> 3b7dd950f58cd8367a0084fcd953f741f4a13c3c
	// search for the pre index that txn follow
	list_for_each_entry_safe(this_txn,tmp,&(txn_list[index].list),txn_list_handle){
		if(this_txn->txn_num < txn->txn_num){
			pre = &(this_txn->txn_list_handle);
		}else{
			break;
		}
	}
	list_add(&(txn->txn_list_handle), pre);

	// check the txn_nums if enough ,if TRUE then handle, post cq and delete corresponding list and free txn
	if(++txn_list[index].txn_nums == txn->txn_length){
		if(txn->type == TXN_READ){
			handle_read(&txn_list[index].list, txn->prps[0], txn->prps[1]);
		}
		post_cqe(&nvme_queues[txn->nvmeq_num], NVME_SC_SUCCESS, txn->req_id, NULL);
		pcie_send_msi(nvme_queues[txn->nvmeq_num].cq_vector);

<<<<<<< HEAD
		list_for_each_entry_safe(this_txn, tmp, &(txn_list[index].list), txn_list_handle){
=======
		list_for_each_entry_safe(this_txn, tmp, &(txn_list[index].list), txn_list_handle)
		{
>>>>>>> 3b7dd950f58cd8367a0084fcd953f741f4a13c3c
			cacheline = this_txn->lpa % CACHE_LINE_NUM;
			update_cache(this_txn,cacheline);
			list_del(&(this_txn->txn_list_handle));
			free_txn_slot(this_txn);
		}
		txn_list[index].req_id = NO_reqId;
		txn_list[index].txn_nums = 0;

		// if wait list is not empty,then put it to this new free list
		if(!list_empty(&(txn_list[wait_index].list))){
			insert_waitlist(index);
		}
	}
}



int main(){
	flash_transaction* txn;

	semi_pcie_setup();
	InitCache(DCache);

	while (is_ReQ_init((struct ReQ*)REQ_1_QUEUE_META_BASE) != 1);

	int i;

	// init 32+1 list
	for(i = 0; i <= WORKLIST_NUM; i++){
		INIT_LIST_HEAD(&txn_list[i].list);
		txn_list[i].req_id = NO_reqId;
		txn_list[i].txn_nums = 0;
	}

	while(1){
		ReQ_fetch((struct ReQ*)REQ_1_QUEUE_META_BASE, (void*)&txn, sizeof(&txn));
		if (txn->nvmeq_num == 0) {
			wrap_admin_txn(&txn);
		} else {
			int free_index = NOforFree;
			flash_transaction *this_txn,*tmp;

			// search which index for insert
			for(i = 0; i < WORKLIST_NUM; i++){
				if(txn_list[i].req_id == NO_reqId && free_index == NOforFree){
					// write down the first list that for free
					free_index = i;
				}else if(txn_list[i].req_id == txn->req_id){
					// if found corresponding index then insert to list
					insert_to_list(i, txn);
					break;
				}
			}

			// if not found corresponding index and there are another free list then insert to free list
			// else insert to wait list
<<<<<<< HEAD
			if(i == WORKLIST_NUM && free_index != NOforFree){
				insert_to_list(free_index, txn);
			}else{
				list_add(&(txn->txn_list_handle), &(txn_list[wait_index].list));
			}
=======
			if(i == work_list && free_index != NOforFree){
				insert_to_list(free_index,&txn);
			}else if(i == work_list && free_index == NOforFree){
				list_add(&(txn.txn_list_handle),&(txn_list[wait_index].list));
			}	
>>>>>>> 3b7dd950f58cd8367a0084fcd953f741f4a13c3c
		}
	}

//	Xil_SetTlbAttributes((UINTPTR)DMA_BDRING_BUFFER, 0x701); // Set 1MB Memory to be uncacheable.
//	semi_pcie_setup();
//	while (is_ReQ_init((struct ReQ*)REQ_1_QUEUE_META_BASE) != 1);
//	while (1) {
//		ReQ_fetch((struct ReQ*)REQ_1_QUEUE_META_BASE, (void*)&txn, sizeof(&txn));
//		if (txn->txn_num == txn->txn_length - 1) {
//			post_cqe(&nvme_queues[txn->nvmeq_num], 0, txn->req_id, NULL); // TODO： just for test
//			pcie_send_msi(nvme_queues[txn->nvmeq_num].cq_vector);
//		}
//		free_txn_slot(txn);
//	}
}
