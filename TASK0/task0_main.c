#include <stdlib.h>
#include <string.h>

#include "xstatus.h"
#include "errno.h"
#include "nvme.h"
#include "hostif.h"
#include "request_types.h"
#include "req_queue.h"
#include "txn_pool.h"
#include "config.h"
#include "iov_iter.h"
#include "intr.h"


int last_queue = -1;

static lpa_t cache_shadow_tag[CACHE_LINE_NUM];
static uint8_t cache_shadow_status[CACHE_LINE_NUM]; //  0-INVAILD 1-VAILD 2-DIRTY

#define CACHELINE_INVAILD 0
#define CACHELINE_VAILD 1
#define CACHELINE_DIRTY 3

struct ReQ* req_queue = (struct ReQ*)REQ_1_QUEUE_META_BASE;

static int segment_user_request(user_request* req){
	flash_transaction *txn;
	unsigned int count = 0;
	unsigned int txn_num = 0;
	lha_t slba = req->start_lba;

	INIT_LIST_HEAD(&req->txn_head); // Init the head of the txn list

	if (req->req_type == IOREQ_FLUSH){
		while ((txn = alloc_txn_slot()) == NULL);
		txn->type = TXN_FLUSH;
		return 0;
	}

	while (count < req->sector_count) {
	    unsigned page_offset = slba % SECTORS_PER_FLASH_PG;
	    unsigned int txn_size = SECTORS_PER_FLASH_PG - page_offset;
	    lpa_t lpa = slba / SECTORS_PER_FLASH_PG;

	    if (count + txn_size > req->sector_count)
	        txn_size = req->sector_count - count;

	    while ((txn = alloc_txn_slot()) == NULL);
	    // xil_printf("[Debug] Txn->data address: %x\n", txn->data);

		txn->txn_num = txn_num;
		txn->type = (req->req_type == IOREQ_WRITE) ? TXN_WRITE : TXN_READ;
		txn->source = TS_USER_IO;
		txn->nsid = req->nsid;
		txn->req_id = req->req_id;
		txn->lpa = lpa;
		txn->ppa = NO_PPA;
		txn->offset = page_offset << SECTOR_SHIFT;
		txn->length = txn_size << SECTOR_SHIFT;
		txn->prps[0] = req->prps[0];
		txn->prps[1] = req->prps[1];
		txn->nvmeq_num = last_queue;
		txn->cmd_type = IO_CMD;

		slba += txn_size;
		count += txn_size;

		INIT_LIST_HEAD(&txn->txn_list_handle); // Init the list handle
		list_add_tail(&(txn->txn_list_handle), &(req->txn_head)); // Append the list handle to the end of the txn list
		txn_num++;
	}
	req->txn_length = txn_num;
	return 0;
}

static void preprocess_txnlist(user_request* req){
	flash_transaction *txn;
	unsigned int cache_line;
	iovec iov[1 << MAX_DATA_TRANSFER_SIZE];
	iovec* iov_ptr = iov;
	iov_iter iter;
	size_t count;
	int i;

	i = 0;
	count = 0;
	list_for_each_entry(txn, &(req->txn_head), txn_list_handle){
		cache_line = txn->lpa % CACHE_LINE_NUM;
		if (cache_shadow_tag[cache_line] == txn->lpa && cache_shadow_status[cache_line] != CACHELINE_INVAILD){
			txn->hit = HIT;
		} else {
			if (cache_shadow_status[cache_line] == CACHELINE_DIRTY){
				txn->hit = MISS_DIRTY;
			} else {
				txn->hit = MISS_NONDIRTY;
			}
		}
		txn->evict_lpa = cache_shadow_tag[cache_line];
		cache_shadow_tag[cache_line] = txn->lpa;
		if (req->req_type == IOREQ_WRITE){
			cache_shadow_status[cache_line] = CACHELINE_DIRTY;
			iov_ptr->iov_base = txn->data + txn->offset;
			iov_ptr->iov_len = txn->length;
			iov_ptr++;
			i++;
			count += txn->length;
		}

		txn->txn_length = req->txn_length;
	}
	if (req->req_type == IOREQ_WRITE){
		iov_iter_init(&iter, iov, i, count);
		nvme_dma_read(req, &iter, count);
	}
}

static void enqueue_txns(user_request* req){
	flash_transaction *txn;

	list_for_each_entry(txn, &(req->txn_head), txn_list_handle) {
		ReQ_append(req_queue, (void*)&txn, sizeof(&txn));
	}
}

static void decept_read(user_request* req){
	flash_transaction *txn, *tmp;
	iovec iov[1 << MAX_DATA_TRANSFER_SIZE];
	iovec* iov_ptr = iov;
	iov_iter iter;
	int i;
	size_t count;

	i = 0;
	count = 0;
	list_for_each_entry(txn, &(req->txn_head), txn_list_handle) {
		memset(txn->data, 0xFF, sizeof(FLASH_PG_BUFFER_SIZE));
		iov_ptr->iov_base = txn->data + txn->offset;
		iov_ptr->iov_len = txn->length;
		iov_ptr++;
		i++;
		count += txn->length;
	}
	iov_iter_init(&iter, iov, i, count);
	nvme_dma_write(req, &iter, count);
	post_cqe(&nvme_queues[last_queue], 0, submission_entry.common.command_id, NULL);

	list_for_each_entry_safe(txn, tmp, &(req->txn_head), txn_list_handle) {
		list_del(&(txn->txn_list_handle));
		free_txn_slot(txn);
	}
}

static void decept_write(user_request* req){
	flash_transaction *txn, *tmp;

	post_cqe(&nvme_queues[last_queue], 0, submission_entry.common.command_id, NULL);
	list_for_each_entry_safe(txn, tmp, &(req->txn_head), txn_list_handle) {
		list_del(&(txn->txn_list_handle));
		free_txn_slot(txn);
	}
}

static void decept_flush(user_request* req){
	post_cqe(&nvme_queues[last_queue], 0, submission_entry.common.command_id, NULL);
}

int main(){
	int Status;
	flash_transaction output;
	user_request req;

	Status = intr_setup();
	if (Status != XST_SUCCESS){
		xil_printf("[Error!] Failed to setup interrupt.\n");
	}
	xil_printf("[Debug] Setup intr.\n");
	Status = pcie_setup();
	if (Status != XST_SUCCESS){
		xil_printf("[Error!] Failed to setup PCIe.\n");
	}
	xil_printf("[Debug] Setup PCIe.\n");
	nvme_init();
	xil_printf("[Debug] Setup NVMe.\n");
	intr_enable();

	init_txn_pool();

	memset(&cache_shadow_tag, 0, sizeof(lpa_t)*CACHE_LINE_NUM);
	memset(&cache_shadow_status, 0, sizeof(uint8_t)*CACHE_LINE_NUM);

	ReQ_init((struct ReQ*)REQ_1_QUEUE_META_BASE, (u8*)REQ_1_QUEUE_DATA_BASE, 128, sizeof(void*));
	xil_printf("[Debug] Waiting for the Q init.\n");
	//while(req_queue->init_flag != 1);
		/* NOP */
	while(1){
		get_nvme_command(&last_queue);
		if (last_queue == 0) { /* Admin Cmd */
			xil_printf("[Debug] Process Admin Command.\n");
			Status = process_admin_command(&submission_entry);
			//output.nvme_cmd_type = 0;
			//output.nvmeq_num = 0;
			//output.Status = Status;
			//output.command_id = submission_entry.common.command_id;
			//output.result_adm = result_adm;
			//ReQ_append(req_queue, (void*)&output, sizeof(output));
			pcie_send_msi(nvme_queues[0].cq_vector);
		} else { /* IO Cmd */
			Status = parse_io_command(&submission_entry, &req);
			if (req.req_type == IOREQ_READ){
				xil_printf("[Debug] IO req type: READ\n");
			} else if (req.req_type == IOREQ_WRITE){
				xil_printf("[Debug] IO req type: WRITE\n");
			} else if (req.req_type == IOREQ_FLUSH){
				xil_printf("[Debug] IO req type: FLUSH\n");
			}
			Status = segment_user_request(&req);
			preprocess_txnlist(&req);


			enqueue_txns(&req);
			xil_printf("[Debug] Pass req.\n");

//			post_cqe(&nvme_queues[last_queue], 0, submission_entry.common.command_id, NULL); // TODO： just for test
//			pcie_send_msi(nvme_queues[last_queue].cq_vector);
		}
	}
}
