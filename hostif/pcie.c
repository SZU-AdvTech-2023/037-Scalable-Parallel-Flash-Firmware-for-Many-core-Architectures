#include "xparameters.h"
#include "xil_exception.h"
#include "xdebug.h"
#include "xil_io.h"
#include "xscugic.h"
#include "xil_mmu.h"
#include "xaxidma.h"
#include "xstatus.h"

#include <errno.h>
#include <stdatomic.h>

#include "hostif.h"
#include "pcie_soft_intf.h"
#include "config.h"
#include "task.h"
#include "intr.h"
#include "utils.h"

#define MAX_PKT_LEN 0x2000 //8KB

/* DMA device definitions */
#define CC_DMA_DEV_ID XPAR_AXIDMA_0_DEVICE_ID
#define RQ_DMA_DEV_ID XPAR_AXIDMA_1_DEVICE_ID

static XAxiDma* cc_axi_dma_ptr = (XAxiDma*)CC_AXI_DMA_BASE; // Need to be mapped to an address.
static XAxiDma* rq_axi_dma_ptr = (XAxiDma*)RQ_AXI_DMA_BASE; // Need to be mapped to an address.
static atomic_flag* rq_lock = (atomic_flag*)RQ_AXI_DMA_LOCK;

#define PCIE_SOFT_INTF_BASE_ADDR XPAR_PCIE_INTF_SOFT_0_BASEADDR

static pcie_soft_intf pcie_intf;

#define CC_RX_INTR_ID XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR
#define CC_TX_INTR_ID XPAR_FABRIC_AXI_DMA_0_MM2S_INTROUT_INTR
#define RQ_RX_INTR_ID XPAR_FABRIC_PCIE_INTF_SOFT_0_RC_INTROUT_INTR
#define RQ_TX_INTR_ID XPAR_FABRIC_AXI_DMA_1_MM2S_INTROUT_INTR

#define RESET_TIMEOUT_COUNTER 10000

#define TLP_REQ_MEMORY_READ 0
#define TLP_REQ_MEMORY_WRITE 1

#define RQ_TLP_HEADER_SIZE 32

static const size_t max_dma_read_payload_size = 4096; /* 1024 DWs, 11 bits dword count in TLP */
static size_t max_dma_write_payload_size = 32768;

typedef struct {
    unsigned long addr;
    u8 at;
    u16 dword_count;
    u8 req_type;
    u16 requester_id;
    u8 tag;
    u8 target_func;
    u8 bar_id;
} completer_request;

typedef struct {
    u8 low_addr;
    u8 at;
    u16 byte_count;
    u16 dword_count;
    u8 completion_status;
    u16 requester_id;
    u8 tag;
} completer_completion;

typedef struct {
    unsigned long addr;
    u8 at;
    size_t byte_count;
    u8 req_type;
    u8 tag;
    u16 completer_id;
} requester_request;

typedef struct {
    u16 low_addr;
    u8 err_code;
    u16 byte_count;
    u16 dword_count;
    u8 request_completed;
    u8 completion_status;
    u16 requester_id;
    u16 completor_id;
    u8 tag;
}  requester_completion;

typedef struct {
    volatile void* rx_buf;
    volatile int rx_count;
    volatile int CoreNum;
} rq_queue_entry;

typedef struct {
    u8 tlp_header[RQ_TLP_HEADER_SIZE];
    u8* data;
    size_t length;
    u8 tag;
    u8 __padding[15];
} rq_buffer_desc;

static u8* cc_tx_buffer = (u8*)CC_TX_BUFFER; //TODO: Allocate a static address for it.

#define TAG_MAX 64 /*  */
//static rq_queue_entry rq_queue[TAG_MAX];
static rq_queue_entry* rq_queue = (rq_queue_entry*)RQ_QUEUE_BASE;
static atomic_flag* rqq_lock = (atomic_flag*)RQ_QUEUE_LOCK;

static volatile size_t* rqes_in_used = (size_t*)RQ_QUEUE_IN_USE;

#define RQ_BUFFER_MAX 16
static rq_buffer_desc rq_buffers[RQ_BUFFER_MAX] __attribute__((aligned(64))); // CORE0 and CORE1 has different address.

static u8* bdring_buffer;
static u8* cc_rx_bdring;
static u8* cc_tx_bdring;
static u8* rq_rx_bdring;
static u8* rq_tx_bdring;

#define CC_RX_BUFFER_MAX
#define RQ_RX_BUFFER_MAX
static u8* cc_rx_buffer = CC_RX_BUFFER_BASE; //TODO: calculate the size
static u8* rq_rx_buffer; //TODO: calculate the size

static int setup_dma(XAxiDma* dma_ptr, u32 deviceID);

static int setup_cc_rx();
static int setup_cc_tx();
static int setup_rq_rx();
static int setup_rq_tx();

static void cc_tx_intr_handler(void* callback);
static void cc_rx_intr_handler(void* callback);
static void rq_tx_intr_handler(void* callback);
static void rq_rx_intr_handler(void* callback);

int pcie_setup(){
	int Status;
	bdring_buffer = (u8*)DMA_BDRING_BUFFER; // TODO Check the address
	xil_printf("[Debug] bdring_buffer address: %x\n", DMA_BDRING_BUFFER);
	xil_printf("%x\n", sizeof(XAxiDma));
	Xil_SetTlbAttributes((UINTPTR)bdring_buffer, 0x701); // Set 1MB Memory to be uncacheable.
	cc_rx_bdring = bdring_buffer + (0 << 18);            /* 256 kB */
	cc_tx_bdring = bdring_buffer + (1 << 18);
	rq_rx_bdring = bdring_buffer + (2 << 18);
	rq_tx_bdring = bdring_buffer + (3 << 18);

	psif_init(&pcie_intf, (void*)PCIE_SOFT_INTF_BASE_ADDR);

	for (int i = 0; i < TAG_MAX; i++){
		rq_queue[i].CoreNum = -1;
	}
	Status = setup_dma(cc_axi_dma_ptr, CC_DMA_DEV_ID);
	if (Status != XST_SUCCESS) {
		xil_printf("[Error!] CC_AXI_DMA initialization failed\n");
		return XST_FAILURE;
	}
	Status = setup_dma(rq_axi_dma_ptr, RQ_DMA_DEV_ID);
	if (Status != XST_SUCCESS) {
		xil_printf("[Error!] RQ_AXI_DMA initialization failed\n");
		return XST_FAILURE;
	}
	Status = setup_cc_rx();
	if (Status != XST_SUCCESS) {
	    xil_printf("[Error!] Failed to setup CC RX buffer descriptor ring\n");
	    return XST_FAILURE;
	}
	xil_printf("[Debug] CC_RX_BUFFER address: %x\n", cc_rx_buffer);
	Status = setup_cc_tx();
	if (Status != XST_SUCCESS) {
	    xil_printf("[Error!] Failed to setup CC TX buffer descriptor ring\n");
	    return XST_FAILURE;
	}
	Status = setup_rq_rx();
	if (Status != XST_SUCCESS) {
		xil_printf("[Error!] Failed to setup RQ RX buffer descriptor ring\n");
	    return XST_FAILURE;
	}
	Status = setup_rq_tx();
	if (Status != XST_SUCCESS) {
	    xil_printf("[Error!] Failed to setup RQ TX buffer descriptor ring\n");
	    return XST_FAILURE;
	}
	xil_printf("[Debug] Setup pcie-dma.\n");
	Status = intr_setup_irq(CC_RX_INTR_ID, 0x3, (Xil_InterruptHandler)cc_rx_intr_handler, XAxiDma_GetRxRing(cc_axi_dma_ptr));
	if (Status != XST_SUCCESS) {
	    xil_printf("[Error!] Failed to setup CC RX IRQ\r\n");
	    return XST_FAILURE;
	}
	Status = intr_setup_irq(RQ_TX_INTR_ID, 0x3, (Xil_InterruptHandler)rq_tx_intr_handler, XAxiDma_GetTxRing(rq_axi_dma_ptr));
	if (Status != XST_SUCCESS) {
	    xil_printf("[Error!] Failed to setup RQ TX IRQ\r\n");
	    return XST_FAILURE;
	}
	Status = intr_setup_irq(RQ_RX_INTR_ID, 0x1, (Xil_InterruptHandler)rq_rx_intr_handler, &pcie_intf);
	if (Status != XST_SUCCESS) {
	    xil_printf("Failed to setup RQ RX IRQ\r\n");
	    return XST_FAILURE;
	}
	xil_printf("[Debug] Setup pcie-intr.\n");
	intr_enable_irq(CC_RX_INTR_ID);
	intr_enable_irq(RQ_TX_INTR_ID);
	intr_enable_irq(RQ_RX_INTR_ID);

    return XST_SUCCESS;
}

int semi_pcie_setup() {
	XAxiDma_BdRing* tx_ring;
	u32 max_transfer_len;
	tx_ring = XAxiDma_GetTxRing(rq_axi_dma_ptr);
	max_transfer_len = tx_ring->MaxTransferLen;

	max_transfer_len = 1 << (31 - clz(max_transfer_len));
	max_dma_write_payload_size = max_transfer_len;

	Xil_SetTlbAttributes((UINTPTR)DMA_BDRING_BUFFER, 0x701); // Set 1MB Memory to be uncacheable.

	psif_init(&pcie_intf, (void*)PCIE_SOFT_INTF_BASE_ADDR);

	return 0;
}

static int setup_dma(XAxiDma* dma_ptr, u32 deviceID){
	XAxiDma_Config* Config = XAxiDma_LookupConfig(deviceID);
	if(!Config){
		xil_printf("[Error!] No config found for %d\n", deviceID);
		return XST_FAILURE;
	}
	int Status = XAxiDma_CfgInitialize(dma_ptr, Config);
	if (Status != XST_SUCCESS) {
		xil_printf("[Error!] Initialization failed %d\n", Status);
		return XST_FAILURE;
	}
	if (!XAxiDma_HasSg(dma_ptr)) {
		xil_printf("[Error!] Device configured as simple mode \n");
	    return XST_FAILURE;
	}
	return XST_SUCCESS;
}

static int rx_setup(XAxiDma* dma_ptr, int is_cc){
	XAxiDma_BdRing* rx_ring;
	u8* rx_bdring;
	u8* rx_buffer;
	XAxiDma_Bd bd_template;
	XAxiDma_Bd* bd_ptr;
	XAxiDma_Bd* bd_cur_ptr;
	int bd_count, free_bd_count, Status;

	rx_bdring = is_cc ? cc_rx_bdring : rq_rx_bdring;
	rx_buffer = is_cc ? cc_rx_buffer : rq_rx_buffer;

	rx_ring = XAxiDma_GetRxRing(dma_ptr);
    XAxiDma_BdRingIntDisable(rx_ring, XAXIDMA_IRQ_ALL_MASK);
    bd_count = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT, 0x1000); // TODO: Here is the rx_bdring size. 64 slots.
    Status = XAxiDma_BdRingCreate(rx_ring, (UINTPTR)rx_bdring, (UINTPTR)rx_bdring, XAXIDMA_BD_MINIMUM_ALIGNMENT, bd_count);
    if (Status != XST_SUCCESS) {
        xil_printf("[Error!] Rx bd create failed with %d\n", Status);
        return XST_FAILURE;
    }
    XAxiDma_BdClear(&bd_template);
    Status = XAxiDma_BdRingClone(rx_ring, &bd_template);
    if (Status != XST_SUCCESS) {
        xil_printf("[Error!] Rx bd clone failed with %d\n", Status);
        return XST_FAILURE;
    }
    free_bd_count = XAxiDma_BdRingGetFreeCnt(rx_ring);
    Status = XAxiDma_BdRingAlloc(rx_ring, free_bd_count, &bd_ptr);
    if (Status != XST_SUCCESS) {
  		xil_printf("[Error!] Rx bd alloc failed with %d\r\n", Status);
    	return XST_FAILURE;
    }
    bd_cur_ptr = bd_ptr;
    for(int index = 0; index < free_bd_count; index++){
    	Status = XAxiDma_BdSetBufAddr(bd_cur_ptr, rx_buffer);
    	if (Status != XST_SUCCESS) {
    		xil_printf("[Error!] Rx set buffer addr %x on BD %x failed %d\n", (unsigned int)rx_buffer, (UINTPTR)bd_cur_ptr, Status);
    		return XST_FAILURE;
    	}
    	Status = XAxiDma_BdSetLength(bd_cur_ptr, MAX_PKT_LEN, rx_ring->MaxTransferLen);
    	if (Status != XST_SUCCESS) {
   			xil_printf("[ERROR!] Rx set length %d on BD %x failed %d\n", MAX_PKT_LEN, (UINTPTR)bd_cur_ptr, Status);
   			return XST_FAILURE;
    	}
    	XAxiDma_BdSetCtrl(bd_cur_ptr, 0);
    	XAxiDma_BdSetId(bd_cur_ptr, rx_buffer);
    	rx_buffer += MAX_PKT_LEN;
    	bd_cur_ptr = (XAxiDma_Bd *)XAxiDma_BdRingNext(rx_ring, bd_cur_ptr);
    }
    Status = XAxiDma_BdRingSetCoalesce(rx_ring, 1, 0);
    Status = XAxiDma_BdRingToHw(rx_ring, free_bd_count, bd_ptr);
    if (Status != XST_SUCCESS) {
    	xil_printf("[Error!] Rx ToHw failed with %d\n", Status);
    	return XST_FAILURE;
    }
    XAxiDma_BdRingIntEnable(rx_ring, XAXIDMA_IRQ_ALL_MASK);
    Status = XAxiDma_BdRingStart(rx_ring);
    if (Status != XST_SUCCESS) {
		xil_printf("[Error!] Rx start BD ring failed with %d\n", Status);
    	return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int setup_cc_rx(){
	return rx_setup(cc_axi_dma_ptr, 1);
}

static int setup_rq_rx(){
	*rqes_in_used = 0;
	return XST_SUCCESS;
}

static int tx_setup(XAxiDma* dma_ptr, int is_cc){
	XAxiDma_BdRing* tx_ring;
	u8* tx_bdring;
	XAxiDma_Bd bd_template;
	int bd_count, Status;

	tx_bdring = is_cc ? cc_tx_bdring : rq_tx_bdring;

	tx_ring = XAxiDma_GetTxRing(dma_ptr);
	XAxiDma_BdRingIntDisable(tx_ring, XAXIDMA_IRQ_ALL_MASK);
	bd_count = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT, (1 << 18)); // TODO: Here is the tx_bdring size. 4096 slots.
	Status = XAxiDma_BdRingCreate(tx_ring, (UINTPTR)tx_bdring, (UINTPTR)tx_bdring, XAXIDMA_BD_MINIMUM_ALIGNMENT, bd_count);
	if (Status != XST_SUCCESS) {
	    xil_printf("[Error!] Tx bd create failed with %d\n", Status);
	    return XST_FAILURE;
	}
	XAxiDma_BdClear(&bd_template);
	Status = XAxiDma_BdRingClone(tx_ring, &bd_template);
	if (Status != XST_SUCCESS) {
	    xil_printf("[Error!] Tx bd clone failed with %d\n", Status);
	    return XST_FAILURE;
	}
	if (!is_cc) XAxiDma_BdRingIntEnable(tx_ring, XAXIDMA_IRQ_ALL_MASK);
	Status = XAxiDma_BdRingStart(tx_ring);
	if (Status != XST_SUCCESS) {
	    xil_printf("[Error!] Tx start BD ring failed with %d\n", Status);
	    return XST_FAILURE;
	}
	return XST_SUCCESS;

}

static int setup_cc_tx(){
	return tx_setup(cc_axi_dma_ptr, 1);
}

static int setup_rq_tx(){
	// setup_max_dma_write_payload_size();
	atomic_flag_clear(rq_lock);
	atomic_flag_clear(rqq_lock);
	return tx_setup(rq_axi_dma_ptr, 0);
}

static int setup_rq(void* buf, size_t rx_count){

	while(atomic_flag_test_and_set(rqq_lock));
	//Xil_AssertNonvoid(*rqes_in_used <= TAG_MAX);
	for (int i = 0; i < TAG_MAX; i++){
		if (rq_queue[i].CoreNum == -1){
			rq_queue[i].CoreNum = A53CORENUM; // TODO: Add a header file for each core which define the A53CORENUM
			rq_queue[i].rx_buf = buf;
			rq_queue[i].rx_count = rx_count;
			Xil_DCacheFlushRange((UINTPTR)buf, rx_count);
			psif_rc_setup_buffer(&pcie_intf, i, buf, rx_count);
			//*rqes_in_used++;
			atomic_flag_clear(rqq_lock);
			return i;
		}
	}
	atomic_flag_clear(rqq_lock);
	return -1;
}

static int complete_rq(u8 tag){
	rq_queue_entry* rqe = rq_queue + tag;

	//XilAssertVoid(rqes_in_used > 0);

	//*rqes_in_used--;
	rqe->CoreNum = -1;
	rqe->rx_buf = NULL;
	rqe->rx_count = 0;
}

static void complete_all_rqs(rq_buffer_desc* rqs, size_t nr_rqs){
	while(atomic_flag_test_and_set(rqq_lock));
	for (int i = 0; i < nr_rqs; i++){
		u8 tag = rqs[i].tag;
		//rq_queue_entry* rqe = rq_queue + tag;
		complete_rq(tag);
	}
	atomic_flag_clear(rqq_lock);
}

static int send_cc_packet_sync(const u8* pkt_buf, size_t pkt_size){
	XAxiDma_BdRing* tx_ring;
	XAxiDma_Bd* bd_ptr;
	int Status, count;

	Xil_DCacheFlushRange((UINTPTR)pkt_buf, pkt_size);
	tx_ring = XAxiDma_GetTxRing(cc_axi_dma_ptr);
	Status = XAxiDma_BdRingAlloc(tx_ring, 1, &bd_ptr);
	if (Status != XST_SUCCESS){
		return XST_FAILURE;
	}
	Status = XAxiDma_BdSetBufAddr(bd_ptr, (UINTPTR)pkt_buf);
	if (Status != XST_SUCCESS){
		return XST_FAILURE;
	}
	Status = XAxiDma_BdSetLength(bd_ptr, pkt_size, tx_ring->MaxTransferLen);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}
	XAxiDma_BdSetCtrl(bd_ptr, XAXIDMA_BD_CTRL_TXEOF_MASK | XAXIDMA_BD_CTRL_TXSOF_MASK);
	XAxiDma_BdSetId(bd_ptr, (UINTPTR)pkt_buf);
	Status = XAxiDma_BdRingToHw(tx_ring, 1, bd_ptr);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}
	while ((count = XAxiDma_BdRingFromHw(tx_ring, XAXIDMA_ALL_BDS, &bd_ptr)) == 0);
	        /* NOP */
	Status = XAxiDma_BdRingFree(tx_ring, count, bd_ptr);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}
	return XST_SUCCESS;
}

static int send_rq_packets_sync(rq_buffer_desc* bufs, size_t count){
	XAxiDma_BdRing* tx_ring;
	XAxiDma_Bd* bd_ptr;
	XAxiDma_Bd* bd_cur_ptr;
	size_t bd_count;
	int Status, r;

	bd_count = 0;
	for (int i = 0; i < count; i++){
		bd_count++;
		if (bufs[i].data){
			bd_count += 1;
		}
	}

	while(atomic_flag_test_and_set(rq_lock));

	tx_ring = XAxiDma_GetTxRing(rq_axi_dma_ptr);
	Status = XAxiDma_BdRingAlloc(tx_ring, bd_count, &bd_ptr);
	if(Status != XST_SUCCESS){
		return ENOSPC;
	}
	bd_cur_ptr = bd_ptr;
	r = EINVAL;
	for (int i = 0; i < count; i++){
		rq_buffer_desc* buf = bufs + i;
		Xil_DCacheFlushRange((UINTPTR)buf->tlp_header, RQ_TLP_HEADER_SIZE);
		Status = XAxiDma_BdSetBufAddr(bd_cur_ptr, (UINTPTR)buf->tlp_header);
		if (Status != XST_SUCCESS) {
			xil_printf("Tx set header addr %p on BD %p failed %d\r\n", buf->tlp_header, bd_cur_ptr, Status);
			goto cleanup;
		}
		Status = XAxiDma_BdSetLength(bd_cur_ptr, RQ_TLP_HEADER_SIZE, tx_ring->MaxTransferLen);
		if (Status != XST_SUCCESS) {
			xil_printf("Tx set length %d on BD %p failed %d\r\n", RQ_TLP_HEADER_SIZE, bd_cur_ptr, Status);
		    goto cleanup;
		}
		XAxiDma_BdSetCtrl(bd_cur_ptr, (buf->data ? 0 : XAXIDMA_BD_CTRL_TXEOF_MASK) | XAXIDMA_BD_CTRL_TXSOF_MASK);
		XAxiDma_BdSetId(bd_cur_ptr, A53CORENUM);
		if (buf->data){
			Xil_DCacheFlushRange((UINTPTR)buf->data, buf->length);
			bd_cur_ptr = (XAxiDma_Bd*)XAxiDma_BdRingNext(tx_ring, bd_cur_ptr);
			Status = XAxiDma_BdSetBufAddr(bd_cur_ptr, (UINTPTR)buf->data);
			if (Status != XST_SUCCESS) {
				xil_printf("Tx set buffer addr %p length %d on BD %p failed %d\r\n", buf->data, buf->length, bd_ptr, Status);
			    goto cleanup;
			}
			Status = XAxiDma_BdSetLength(bd_cur_ptr, buf->length, tx_ring->MaxTransferLen);
			if (Status != XST_SUCCESS){
				xil_printf("Tx set length %d on BD %p failed %d\r\n", buf->length, bd_cur_ptr, Status);
				goto cleanup;
			}
			XAxiDma_BdSetCtrl(bd_cur_ptr, XAXIDMA_BD_CTRL_TXEOF_MASK);
			XAxiDma_BdSetId(bd_cur_ptr, A53CORENUM);
		}
		bd_cur_ptr = (XAxiDma_Bd*)XAxiDma_BdRingNext(tx_ring, bd_cur_ptr);
	}
	r = EIO;

	core_inf_array* CoreInfs =(core_inf_array*)COREINF_SPACE_BASE;
	CoreInfs->cores[A53CORENUM].pending_rqs = bd_count;
	CoreInfs->cores[A53CORENUM].rqs_runable = 0;

	Status = XAxiDma_BdRingToHw(tx_ring, bd_count, bd_ptr);
	smp_wmb(); // Very Important for the Intr Handler on Core0!!
	if (Status != XST_SUCCESS){
		xil_printf("To Hw failed %d\r\n", Status);
		goto cleanup;
	}

	while(CoreInfs->cores[A53CORENUM].rqs_runable != 1);
		/* NOP */
	atomic_flag_clear(rq_lock);
	r = 0;
	return r;

cleanup:
	XAxiDma_BdRingFree(tx_ring, bd_count, bd_ptr);
	return r;
}

static size_t pack_requester_request(const requester_request* req, const u8* buffer, size_t count, rq_buffer_desc* rq_buf){
    u32* dws = (u32*)rq_buf->tlp_header;
    size_t out_size;

    memset(rq_buf, 0, sizeof(*rq_buf));

    dws[0] = (req->addr & 0xfffffffc) | (req->at & 0x3);
    dws[1] = (req->addr >> 32) & 0xffffffff;
    dws[2] = (u32)req->byte_count;
    dws[3] = req->tag;
    dws[3] |= req->completer_id << 8;
    dws[3] |= (req->req_type & 0xf) << 24;

    out_size = 32;

    if (buffer) {
        if (count <= 16) {
            /* Piggybacking */
            *(u64*)&rq_buf->tlp_header[16] = *(u64*)buffer;
            *(u64*)&rq_buf->tlp_header[24] = *(u64*)&buffer[8];
        } else {
            rq_buf->data = (u8*)buffer;
            rq_buf->length = count;
            out_size += count;
        }
    }

    rq_buf->tag = req->tag;

    return out_size;
}

static size_t unpack_completer_request(const u8* buffer, completer_request* cr){
    const volatile u32* dws = (const volatile u32*)buffer;
    u8 bar_aperture;
    unsigned long mask;

    cr->bar_id = (dws[3] >> 16) & 0x7;
    bar_aperture = (dws[3] >> 19) & 0x3f;
    cr->addr = (((unsigned long)dws[1] << 32UL) | dws[0]) & ~0x3UL;
    mask = (1ULL << bar_aperture) - 1;
    cr->addr = cr->addr & mask;
    cr->at = dws[0] & 0x3;
    cr->requester_id = (dws[2] >> 16) & 0xffff;
    cr->dword_count = dws[2] & 0x7ff;
    cr->req_type = (dws[2] >> 11) & 0xf;
    cr->tag = dws[3] & 0x3f; /* 6-bit tags */
    cr->target_func = (dws[3] >> 8) & 0xff;

    return 16;
}

static int pcie_do_dma_iter(unsigned long addr, int do_write, iov_iter* iter, size_t count){
	requester_request req = {.at = 0, .completer_id = 0};
	size_t max_payload_size;
	size_t nbufs = 0;
	int tag = 0;
	int r;

	core_inf_array* CoreInfs =(core_inf_array*)COREINF_SPACE_BASE;
	max_payload_size = do_write ? max_dma_write_payload_size : max_dma_read_payload_size;
	while (count > 0){
		rq_buffer_desc* rq_buf;
		void* buf = NULL;
		size_t chunk = count;
		if (chunk > max_payload_size){
			chunk = max_payload_size;
		}
		iov_iter_get_bufaddr(iter, &buf, &chunk);
		// TODO: Is the alignment check necessary at here?
		iov_iter_consume(iter, chunk);
		if (!do_write){
			tag = setup_rq(buf, chunk);
		}
		if (nbufs >= RQ_BUFFER_MAX || (!do_write && tag < 0)){
			if(!do_write){
				CoreInfs->cores[A53CORENUM].pending_rcs = nbufs;
				CoreInfs->cores[A53CORENUM].rcs_runable = 0;
			}
			r = send_rq_packets_sync(rq_buffers, nbufs);
			if (r){
				if (!do_write){
					complete_all_rqs(rq_buffers, nbufs);
				}
				return r;
			}
			if (!do_write){
				while(CoreInfs->cores[A53CORENUM].rcs_runable != 1);
					/* NOP */
				complete_all_rqs(rq_buffers, nbufs);
			}
			nbufs = 0;
			if (!do_write && tag < 0){
				tag = setup_rq(buf, chunk);
			}
		}
		rq_buf = rq_buffers + nbufs;
		nbufs++;
		req.tag = tag;
		req.req_type = do_write ? TLP_REQ_MEMORY_WRITE : TLP_REQ_MEMORY_READ;
		req.addr = addr;
		req.byte_count = chunk;
		pack_requester_request(&req, do_write ? buf : NULL, chunk, rq_buf);
		count -= chunk;
		addr += chunk;
	}
	if (nbufs > 0){
		if(!do_write){
			CoreInfs->cores[A53CORENUM].pending_rcs = nbufs;
			CoreInfs->cores[A53CORENUM].rcs_runable = 0;
		}
		r = send_rq_packets_sync(rq_buffers, nbufs);
		if (r){
			if (!do_write){
				complete_all_rqs(rq_buffers, nbufs);
			}
			return r;
		}
		if (!do_write){
			while(CoreInfs->cores[A53CORENUM].rcs_runable != 1);
				/* NOP */
			complete_all_rqs(rq_buffers, nbufs);
		}
	}
	return 0;
}

int pcie_dma_read_iter(unsigned long addr, iov_iter* iter, size_t count){
	return pcie_do_dma_iter(addr, 0, iter, count);
}

int pcie_dma_write_iter(unsigned long addr, iov_iter* iter, size_t count){
	return pcie_do_dma_iter(addr, 1, iter, count);
}

int pcie_dma_read(unsigned long addr, __u8* buffer, size_t count){
	iovec iov = {.iov_base = buffer, .iov_len = count};
	iov_iter iter;
	iov_iter_init(&iter, &iov, 1, count);
	return pcie_dma_read_iter(addr, &iter, count);
}

int pcie_dma_write(unsigned long addr, const u8* buffer, size_t count){
	iovec iov = {.iov_base = buffer, .iov_len = count};
	iov_iter iter;
	iov_iter_init(&iter, &iov, 1, count);
	return pcie_dma_write_iter(addr, &iter, count);
}

static size_t pack_completer_completion(const completer_completion* cc, const u8* buffer, size_t count, u8* out_buffer){
    u32* dws = (u32*)out_buffer;

    dws[0] = cc->low_addr & 0x7f;
    dws[0] |= (cc->at & 0x3) << 8;
    dws[0] |= (cc->byte_count & 0x1fff) << 16;
    dws[1] = cc->dword_count & 0x7ff;
    dws[1] |= (cc->completion_status & 0x7) << 11;
    dws[1] |= cc->requester_id << 16;
    dws[2] = cc->tag;

    memcpy(&out_buffer[12], buffer, count);

    return 12 + count;
}

int pcie_send_completion(unsigned long addr, u16 requester_id, u8 tag, const u8* buffer, size_t count){
	completer_completion cc = {
		.low_addr = (u8)(addr & 0xff),
		.at = 0,
		.byte_count = count,
		.dword_count = count >> 2,
		.completion_status = 0,
		.requester_id = requester_id,
		.tag = tag,
	};
	u8* tlp_buf = cc_tx_buffer;
	size_t tlp_size;
	int r;

	tlp_size = pack_completer_completion(&cc, buffer, count, tlp_buf);
	r = send_cc_packet_sync(tlp_buf, tlp_size);
	return r;
}

static void handle_host_req(u8* buffer){
	completer_request cr;
	int data_offset;

	data_offset = unpack_completer_request(buffer, &cr);
	if (cr.req_type == TLP_REQ_MEMORY_READ){
		nvme_handle_reg_read(cr.addr, cr.requester_id, cr.tag, cr.dword_count << 2);
	} else if (cr.req_type == TLP_REQ_MEMORY_WRITE) {
		nvme_handle_reg_write(cr.addr, buffer + data_offset, cr.dword_count << 2);
	}
}

static void cc_rx_callback(XAxiDma_BdRing* cc_rx_ring){
	int bd_count, free_bd_count;
	XAxiDma_Bd* bd_ptr;
	XAxiDma_Bd* bd_cur_ptr;
	u32 bd_status;
	void* buffer;
	int Status;

	//xil_printf("[Debug] CC_RX_CALLBACK: handle host pcie request.\n");

	bd_count = XAxiDma_BdRingFromHw(cc_rx_ring, XAXIDMA_ALL_BDS, &bd_ptr);
	bd_cur_ptr = bd_ptr;
	for (int i = 0; i < bd_count; i++){
		bd_status = XAxiDma_BdGetSts(bd_cur_ptr);
		buffer = (void*)XAxiDma_BdGetBufAddr(bd_cur_ptr);
		if ((bd_status & XAXIDMA_BD_STS_ALL_ERR_MASK) || (!(bd_status & XAXIDMA_BD_STS_COMPLETE_MASK))){
			xil_printf("[Error!] RX callback error\n");
		} else {
			Xil_DCacheInvalidateRange((UINTPTR)buffer, MAX_PKT_LEN);
			handle_host_req(buffer);
		}
		bd_cur_ptr = (XAxiDma_Bd*)XAxiDma_BdRingNext(cc_rx_ring, bd_cur_ptr);
	}
	if (bd_count > 0){
		Status = XAxiDma_BdRingFree(cc_rx_ring, bd_count, bd_ptr);
		if (Status != XST_SUCCESS){
			xil_printf("[Error!] RX BdRing free failed!\n");
		}
		free_bd_count = XAxiDma_BdRingGetFreeCnt(cc_rx_ring);
		Status = XAxiDma_BdRingAlloc(cc_rx_ring, free_bd_count, &bd_ptr);
		if (Status != XST_SUCCESS){
			xil_printf("[Error!] RX BdRing Bd alloc failed!\n");
		}
		Status = XAxiDma_BdRingToHw(cc_rx_ring, free_bd_count, bd_ptr);
		if (Status != XST_SUCCESS){
			xil_printf("[Error!] RX BdRing Bd submit failed!\n");
		}
	}
}

static void rq_tx_callback(XAxiDma_BdRing* rq_tx_ring){
	int bd_count;
	XAxiDma_Bd* bd_ptr;
	XAxiDma_Bd* bd_cur_ptr;
	u32 bd_status, CoreNum;
	int Status;

	xil_printf("[Debug] RQ_TX_CALLBACK.\n");
	core_inf_array* CoreInfs =(core_inf_array*)COREINF_SPACE_BASE;
	//while ((bd_count = XAxiDma_BdRingFromHw(rq_tx_ring, XAXIDMA_ALL_BDS, &bd_ptr)) == 0);
	//xil_printf("[Debug] RingPtr.HwCnt: %d\n", rq_tx_ring->HwCnt);
	bd_count = XAxiDma_BdRingFromHw(rq_tx_ring, XAXIDMA_ALL_BDS, &bd_ptr);
	bd_cur_ptr = bd_ptr;
	for (int i = 0; i < bd_count; i++){
		bd_status = XAxiDma_BdGetSts(bd_cur_ptr);
		CoreNum = XAxiDma_BdGetId(bd_cur_ptr);
		//xil_printf("[Debug] Handle task %d\n", CoreNum);
		if ((bd_status & XAXIDMA_BD_STS_ALL_ERR_MASK) || (!(bd_status & XAXIDMA_BD_STS_COMPLETE_MASK))){
			xil_printf("[Error!] TX callback error\n");
		}
		if (--CoreInfs->cores[CoreNum].pending_rqs == 0){
			CoreInfs->cores[CoreNum].rqs_runable = 1;
		}
		bd_cur_ptr = (XAxiDma_Bd*)XAxiDma_BdRingNext(rq_tx_ring, bd_cur_ptr);
	}
	if (bd_count > 0){
		Status = XAxiDma_BdRingFree(rq_tx_ring, bd_count, bd_ptr);
		if (Status != XST_SUCCESS){
			xil_printf("[Error!] TX BdRing free failed!\n");
		}
	}
}

static void rq_rx_callback(int tag, int error){
	rq_queue_entry* rqe;

	// xil_printf("[Debug] RQ_RX_CALLBACK.\n");
	rqe = rq_queue + tag;
	Xil_DCacheInvalidateRange((UINTPTR)rqe->rx_buf, rqe->rx_count);
	core_inf_array* CoreInfs =(core_inf_array*)COREINF_SPACE_BASE;
	int CoreNum = rqe->CoreNum;
	if (--CoreInfs->cores[CoreNum].pending_rcs == 0){
		CoreInfs->cores[CoreNum].rcs_runable = 1;
	}
}

static void cc_rx_intr_handler(void* callback){
	XAxiDma_BdRing* rx_ring = (XAxiDma_BdRing*)callback;
	u32 IrqStatus;
	int TimeOut;

	//xil_printf("[Debug] Get into CC_RX_INTR_HANDLER\n");

	IrqStatus = XAxiDma_BdRingGetIrq(rx_ring);
	XAxiDma_BdRingAckIrq(rx_ring, IrqStatus);
	if (!(IrqStatus & XAXIDMA_IRQ_ALL_MASK)) {
		return;
	}
    if ((IrqStatus & XAXIDMA_IRQ_ERROR_MASK)) {
        XAxiDma_BdRingDumpRegs(rx_ring);
        XAxiDma_Reset(cc_axi_dma_ptr);
        TimeOut = RESET_TIMEOUT_COUNTER;
        while (TimeOut) {
            if (XAxiDma_ResetIsDone(cc_axi_dma_ptr)) {
                break;
            }
            TimeOut -= 1;
        }
        return;
    }
    if (IrqStatus & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK)) {
        cc_rx_callback(rx_ring);
    }
}

static void rq_tx_intr_handler(void* callback){
	XAxiDma_BdRing* tx_ring = (XAxiDma_BdRing*)callback;
	u32 IrqStatus;
    int TimeOut;

    //xil_printf("[Debug] Get into handler.\n");

    IrqStatus = XAxiDma_BdRingGetIrq(tx_ring);
    XAxiDma_BdRingAckIrq(tx_ring, IrqStatus);
    if (!(IrqStatus & XAXIDMA_IRQ_ALL_MASK)) {
    	//xil_printf("[Debug] Err 0\n");
        return;
    }
    if ((IrqStatus & XAXIDMA_IRQ_ERROR_MASK)) {
        XAxiDma_BdRingDumpRegs(tx_ring);
        XAxiDma_Reset(rq_axi_dma_ptr);
        TimeOut = RESET_TIMEOUT_COUNTER;
        while (TimeOut) {
            if (XAxiDma_ResetIsDone(rq_axi_dma_ptr)) {
                break;
            }
            TimeOut -= 1;
        }
        //xil_printf("[Debug] Err 1\n");
        return;
    }
    if (IrqStatus & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK)) {
        rq_tx_callback(tx_ring);
    } else {
    	//xil_printf("[Debug] Not really handle the intr\n");
    }
}

static void rq_rx_intr_handler(void* callback){
	pcie_soft_intf* psif = (pcie_soft_intf*) callback;
	u64 ioc_bitmap, err_bitmap;

	ioc_bitmap = psif_get_ioc_bitmap(psif);
	err_bitmap = psif_get_err_bitmap(psif);
	psif_rc_ack_intr(psif, ioc_bitmap | err_bitmap);
	for(int i = 0; i < TAG_MAX; i++){
		if((ioc_bitmap & (1ULL << i)) || (err_bitmap & (1ULL << i))){
			rq_rx_callback(i, !!(err_bitmap & (1ULL << i)));
		}
	}
}

int pcie_send_msi(u16 vector){
	int r;

	while(atomic_flag_test_and_set(rqq_lock));
	r = psif_send_msi(&pcie_intf, vector);
	atomic_flag_clear(rqq_lock);

	return r;
}
