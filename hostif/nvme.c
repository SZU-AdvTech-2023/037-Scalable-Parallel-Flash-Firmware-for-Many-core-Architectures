#include "xil_cache.h"

#include "hostif.h"
#include "pcie_soft_intf.h"
#include "config.h"
#include "nvme.h"
#include "utils.h"
#include "request_types.h"

#define NAME "[NVMe]"
#define INVALID_QID ((u16)-1)

typedef struct {
    size_t page_size;
    int cap_mqes;
    int cap_dstrd;
    int cap_mpsmin;
    int cap_mpsmax;
    int cc_en;
    int cc_shn;
    int cc_iosqes;
    int cc_iocqes;
    int cc_mps;
    int csts_shst;
} nvme_controller;

static nvme_controller nvme_ctlr = {
    .page_size = PG_SIZE,
    .cap_mqes = 1024,
    .cap_dstrd = 0,
    .cap_mpsmin = 0,
    .cap_mpsmax = 0,
    .cc_en = 0,
};

nvme_queue* nvme_queues = (nvme_queue*)NVMEQS_BASE; // 1 + CONFIG_NVME_IO_QUEUE_MAX
static unsigned int io_queue_count;

static __u8 prp_list_buf[1 << 12];
struct nvme_command submission_entry __attribute__((aligned(64)));
struct nvme_completion completion_entry __attribute__((aligned(64)));
union nvme_result result_adm;


static int err2statuscode(int err){
	switch (err){
	case 0:
		return NVME_SC_SUCCESS;
		break;
	case 1:
		return NVME_SC_INTERNAL;
		break;
	}
}

void nvme_init(){
    for (int i = 0; i <= CONFIG_NVME_IO_QUEUE_MAX; i++)
        nvme_queues[i].qid = INVALID_QID;

    nvme_queues[0].cq_vector = 0;
}

static void init_nvme_queue(nvme_queue* nvmeq, unsigned int qid){
	xil_printf("[Debug] Init NVMe Queue: %d\n", qid);
    nvmeq->qid = qid;
    nvmeq->sq_head = 0;
    nvmeq->sq_tail = 0;
    nvmeq->cq_head = 0;
    nvmeq->cq_tail = 0;
    nvmeq->cq_phase = 1;
}

static void reset_controller(void) { nvme_ctlr.csts_shst = 0; }

static void update_cq_head_doorbell(unsigned int qid, u32 val){
    nvme_queue* nvmeq;

    if (qid > CONFIG_NVME_IO_QUEUE_MAX) return;

    nvmeq = &nvme_queues[qid];
    if (nvmeq->qid == INVALID_QID) return;

    nvmeq->cq_head = val;
}

static void update_sq_tail_doorbell(unsigned int qid, u32 val){
    nvme_queue* nvmeq;

    if (qid > CONFIG_NVME_IO_QUEUE_MAX) return;

    nvmeq = &nvme_queues[qid];
    if (nvmeq->qid == INVALID_QID) return;

    nvmeq->sq_tail = val;
}

static void nvme_handle_reg_read_raw(unsigned long addr, u16 requester_id, u8 tag, size_t len, u8* out_buf){
    unsigned int result;

    switch (addr) {
    case NVME_REG_CAP:
        result = ((nvme_ctlr.cap_mqes - 1) & 0xffff);
        break;
    case NVME_REG_CAP + 4:
        result = ((unsigned long)nvme_ctlr.cap_dstrd & 0xf) |
                 (((unsigned long)nvme_ctlr.cap_mpsmin & 0xf) << 16UL) |
                 (((unsigned long)nvme_ctlr.cap_mpsmax & 0xf) << 20UL);
        break;
    case NVME_REG_VS:
        result = NVME_VS(1, 1, 0);
        break;
    case NVME_REG_CSTS:
        result = (nvme_ctlr.cc_en ? NVME_CSTS_RDY : 0) | nvme_ctlr.csts_shst;
        break;
    case NVME_REG_CC:
    	result = 0;
    	if (nvme_ctlr.cc_en) result |= NVME_CC_ENABLE;
    	result |= nvme_ctlr.cc_shn;
    	result |= nvme_ctlr.cc_iocqes << NVME_CC_IOCQES_SHIFT;
    	result |= nvme_ctlr.cc_iosqes << NVME_CC_IOSQES_SHIFT;
    	result |= nvme_ctlr.cc_mps << NVME_CC_MPS_SHIFT;
        break;
    case NVME_REG_CMBLOC:
    	result = 0;
    case NVME_REG_CMBSZ:
        result = 0;
        break;
    default:
        result = ~0;
        break;
    }

    // xil_printf("[Debug] nvme reg read %x %d %x\n", addr, len, result);

    *(unsigned int*)out_buf = result;
}

void nvme_handle_reg_read(unsigned long addr, u16 requester_id, u8 tag, size_t len){
    u8 out_buf[8];

    if (len == 4)
    	nvme_handle_reg_read_raw(addr, requester_id, tag, 4, out_buf);
    else if (len == 8) {
    	nvme_handle_reg_read_raw(addr, requester_id, tag, 4, out_buf);
    	nvme_handle_reg_read_raw(addr+4, requester_id, tag, 4, out_buf+4);
    } else {
    	xil_printf("invalid read size: %d\n", len);
    	return;
    }

    // xil_printf("[Debug] nvme reg read %x %d %x\n", addr, len, *(unsigned long long*)out_buf);

    pcie_send_completion(addr, requester_id, tag, out_buf, len);
}

void nvme_handle_reg_write(unsigned long addr, const u8* buf, size_t len){
    u32 val;
    int offset;

    if (len == 4){
        val = *(u32*)buf;
    } else if (len == 8){
    	nvme_handle_reg_write(addr, buf, 4);
		nvme_handle_reg_write(addr+4, buf+4, 4);
		return;
    } else {
        xil_printf("invalid write size: %d", len);
        return;
    }

    // xil_printf("[Debug] nvme reg write %x %d %x\n", addr, len, val);

    switch (addr) {
    case NVME_REG_CC:
        if (nvme_ctlr.cc_en && !(val & NVME_CC_ENABLE)) reset_controller();

        nvme_ctlr.cc_en = !!(val & NVME_CC_ENABLE);
        nvme_ctlr.cc_shn = val & NVME_CC_SHN_MASK;
        nvme_ctlr.cc_iocqes = (val >> NVME_CC_IOCQES_SHIFT) & 0xf;
        nvme_ctlr.cc_iosqes = (val >> NVME_CC_IOSQES_SHIFT) & 0xf;
        nvme_ctlr.cc_mps = (val >> NVME_CC_MPS_SHIFT) & 0xf;

        if (nvme_ctlr.cc_shn != NVME_CC_SHN_NONE) {
            /* Initiate controller shutdown. */
            nvme_ctlr.csts_shst = NVME_CSTS_SHST_OCCUR;
        }
        break;
    case NVME_REG_AQA:
        /* Admin submission queue attribute */
        nvme_queues[0].sq_depth = (val & 0xfff) + 1;
        nvme_queues[0].cq_depth = ((val >> 16) & 0xfff) + 1;
        init_nvme_queue(&nvme_queues[0], 0);
        break;
    case NVME_REG_ASQ:
    case NVME_REG_ASQ + 4:
        /* Admin submission queue base address */
        offset = addr - NVME_REG_ASQ;

        nvme_queues[0].sq_dma_addr =
            (nvme_queues[0].sq_dma_addr & ~(0xffffffffUL << (offset << 3))) |
            ((u64)val << (offset << 3));
        init_nvme_queue(&nvme_queues[0], 0);
        break;
    case NVME_REG_ACQ:
    case NVME_REG_ACQ + 4:
        /* Admin completion queue base address */
        offset = addr - NVME_REG_ACQ;

        nvme_queues[0].cq_dma_addr =
            (nvme_queues[0].cq_dma_addr & ~(0xffffffffUL << (offset << 3))) |
            ((u64)val << (offset << 3));
        init_nvme_queue(&nvme_queues[0], 0);
        break;
    default:
        if (addr >= NVME_REG_DBS) {
            u64 offset = addr - NVME_REG_DBS;

            if (offset & ((1 << (nvme_ctlr.cap_dstrd + 3)) - 1)) {
                update_cq_head_doorbell(offset >> (nvme_ctlr.cap_dstrd + 3),
                                        val);
            } else {
                update_sq_tail_doorbell(offset >> (nvme_ctlr.cap_dstrd + 3),
                                        val);
            }
        }

        break;
    }
}

static nvme_queue* get_queue(int* last_queue){
	nvme_queue* nvmeq;
	int i;

	i = *last_queue;
	while(1){
		i++;
		i = i % CONFIG_NVME_IO_QUEUE_MAX;
		nvmeq = &nvme_queues[i];
		if (nvmeq->qid == INVALID_QID) continue;
		if (nvmeq->sq_head != nvmeq->sq_tail){
			*last_queue = i;
			return nvmeq;
		}
	}
}

static void fetch_next_request(nvme_queue* nvmeq, struct nvme_command* cmd){
    u16 head = nvmeq->sq_head;

    nvmeq->sq_head++;
    if (nvmeq->sq_head == nvmeq->sq_depth) nvmeq->sq_head = 0;

    pcie_dma_read(nvmeq->sq_dma_addr + (head << nvme_ctlr.cc_iosqes), (u8*)cmd, sizeof(*cmd));
}

int post_cqe(nvme_queue* nvmeq, int status, uint16_t command_id, union nvme_result* result){
    struct nvme_completion* cqe = &completion_entry;
    u16 cq_tail;

    /* xil_printf("cqe cid %d status %x\n", command_id, status); */
    memset(cqe, 0, sizeof(*cqe));
    cqe->status = (status << 1) | (nvmeq->cq_phase & 1);
    cqe->command_id = command_id;
    cqe->sq_id = nvmeq->qid;
    cqe->sq_head = nvmeq->sq_head;
    if (result) cqe->result = *result;

    cq_tail = nvmeq->cq_tail;

    if (++nvmeq->cq_tail == nvmeq->cq_depth) {
        nvmeq->cq_tail = 0;
        nvmeq->cq_phase ^= 1;
    }

    //xil_printf("[Debug] nvme_ctl.cc_iocqes: %d", nvme_ctlr.cc_iocqes);
    //pcie_dma_write(nvmeq->cq_dma_addr + (cq_tail << nvme_ctlr.cc_iocqes), (u8*)cqe, sizeof(*cqe));
    pcie_dma_write(nvmeq->cq_dma_addr + (cq_tail << 4), (u8*)cqe, sizeof(*cqe));

    return 0;
}

static inline int flush_dma(int do_write, __u64 addr, iov_iter* iter, size_t count){
    int r;

    if (do_write)
        r = pcie_dma_write_iter(addr, iter, count);
    else
        r = pcie_dma_read_iter(addr, iter, count);

    return r;
}

static int transfer_prp_data(iov_iter* iter, int do_write, __u64 prp1, __u64 prp2, size_t count){
    size_t chunk;
    unsigned int offset;
    __u64* prp_list = (__u64*)prp_list_buf;
    unsigned int nprps;
    __u64 dma_addr;
    size_t dma_size;
    int i, r;

    if(count == 0) return 0;

    /* PRP 1 ,host_page_size is 4K*/
    offset = prp1 % nvme_ctlr.page_size;
    chunk = min(nvme_ctlr.page_size - offset, count);

    if (chunk > 0) {
        dma_addr = prp1;
        dma_size = chunk;

        count -= chunk;
    }

    if (count == 0) return flush_dma(do_write, dma_addr, iter, dma_size);

    /* PRP 2 */
    if (count <= nvme_ctlr.page_size) {
        chunk = count;

        if (dma_addr + dma_size == prp2) {
            dma_size += chunk;
        } else {
            r = flush_dma(do_write, dma_addr, iter, dma_size);
            if (r != 0) return r;

            dma_addr = prp2;
            dma_size = chunk;
        }

        return flush_dma(do_write, dma_addr, iter, dma_size);
    }

    /* PRP list */
    offset = prp2 % nvme_ctlr.page_size;
    nprps = roundup(count, nvme_ctlr.page_size) / nvme_ctlr.page_size + 1;
    nprps = (nprps + 3) & ~0x3; /* Align to 32 bytes */
    nprps = min(nprps, (nvme_ctlr.page_size - offset) >> 3);
    for (i = 0; i < nprps; i++) prp_list[i] = 0;
    Xil_DCacheFlushRange((UINTPTR)prp_list, (nprps << 3));
    r = pcie_dma_read(prp2, (__u8*)prp_list, nprps << 3);
    if (r != 0) return r;

    i = 0;
    for (;;) {
        if (i == nprps - 1) {
            if (count > nvme_ctlr.page_size) {
                /* More PRP lists. */
                //Xil_AssertNonvoid(!(prp_list[i] & (nvme_ctlr.page_size - 1)));
                nprps = roundup(count, nvme_ctlr.page_size) / nvme_ctlr.page_size + 1;
                nprps = (nprps + 3) & ~0x3; /* Align to 32 bytes */
                nprps = min(nprps, nvme_ctlr.page_size >> 3);
                r = pcie_dma_read(prp_list[i], (__u8*)prp_list, nprps << 3);
                if (r != 0) return r;
                i = 0;
            }
        }

        offset = prp_list[i] % nvme_ctlr.page_size;
        chunk = min(nvme_ctlr.page_size - offset, count);

        if (dma_addr + dma_size == prp_list[i]) {
            dma_size += chunk;
        } else {
            r = flush_dma(do_write, dma_addr, iter, dma_size);
            if (r != 0) return r;

            dma_addr = prp_list[i];
            dma_size = chunk;
        }

        i++;
        count -= chunk;

        if (count == 0) break;
    }

    if (dma_size > 0) {
        r = flush_dma(do_write, dma_addr, iter, dma_size);
        if (r != 0) return r;
    }

    return 0;
}

static inline int read_prp_data(iov_iter* iter, u64 prp1, u64 prp2, size_t count){
    return transfer_prp_data(iter, 0, prp1, prp2, count);
}

int write_prp_data(iov_iter* iter, __u64 prp1, __u64 prp2, size_t count){
    return transfer_prp_data(iter, 1, prp1, prp2, count);
}

int nvme_dma_read(user_request* req, iov_iter* iter, size_t count){
    return read_prp_data(iter, req->prps[0], req->prps[1], count);
}

int nvme_dma_write(user_request* req, iov_iter* iter, size_t count){
    return write_prp_data(iter, req->prps[0], req->prps[1], count);
}

static int process_set_features_command(struct nvme_features* cmd, union nvme_result* result){
    int status = NVME_SC_SUCCESS;

    switch (cmd->fid) {
    case NVME_FEAT_NUM_QUEUES:
        result->u32 = CONFIG_NVME_IO_QUEUE_MAX - 1;
        result->u32 |= result->u32 << 16;
        break;
    default:
        status = NVME_SC_FEATURE_NOT_SAVEABLE;
        break;
    }

    return status;
}

static int process_identify_command(struct nvme_identify* cmd, union nvme_result* result){
    u8* identify_data;
    iovec iov;
    iov_iter iter;

    identify_data = (u8*)IDENTIFY_DATA_BASE;

    switch (cmd->cns) {
    case NVME_ID_CNS_NS:
        nvme_identify_namespace(cmd->nsid, identify_data);
        break;
    case NVME_ID_CNS_CTRL:
        nvme_identify_controller(identify_data);
        break;
    case NVME_ID_CNS_NS_ACTIVE_LIST:
        nvme_identify_ns_active_list(identify_data);
        break;
    case NVME_ID_CNS_CS_CTRL:
        nvme_identify_cs_controller(cmd->csi, identify_data);
        break;
    }

    iov.iov_base = identify_data;
    iov.iov_len = 0x1000;
    iov_iter_init(&iter, &iov, 1, 0x1000);

    write_prp_data(&iter, cmd->dptr.prp1, cmd->dptr.prp2, 0x1000);

    return 0;
}

static int process_create_cq_command(struct nvme_create_cq* cmd, union nvme_result* result){
    uint16_t qid = cmd->cqid;
    nvme_queue* nvmeq;

    /* Invalid queue identifier. */
    if (qid == 0 || qid > CONFIG_NVME_IO_QUEUE_MAX) return NVME_SC_QID_INVALID;

    nvmeq = &nvme_queues[qid];

    nvmeq->cq_depth = cmd->qsize + 1;
    nvmeq->cq_dma_addr = cmd->prp1;
    nvmeq->cq_vector = cmd->irq_vector;

    return 0;
}

static int process_create_sq_command(struct nvme_create_sq* cmd, union nvme_result* result){
    uint16_t qid = cmd->sqid;
    nvme_queue* nvmeq;

    /* Invalid queue identifier. */
    if (qid == 0 || qid > CONFIG_NVME_IO_QUEUE_MAX) return NVME_SC_QID_INVALID;
    /* Invalid CQ identifier. */
    if (qid != cmd->cqid) return NVME_SC_CQ_INVALID;

    nvmeq = &nvme_queues[qid];

    /* CQ not created. */
    if (!nvmeq->cq_depth) return NVME_SC_CQ_INVALID;

    nvmeq->sq_depth = cmd->qsize + 1;
    nvmeq->sq_dma_addr = cmd->prp1;

    init_nvme_queue(nvmeq, qid);

    io_queue_count++;

    return 0;
}

int process_admin_command(struct nvme_command* cmd){
    int status = 0;
    result_adm.u64 = 0;

    switch (cmd->common.opcode) {
    case nvme_admin_identify:
        status = process_identify_command(&cmd->identify, &result_adm);
        break;
    case nvme_admin_set_features:
        status = process_set_features_command(&cmd->features, &result_adm);
        break;
    case nvme_admin_create_cq:
        status = process_create_cq_command(&cmd->create_cq, &result_adm);
        break;
    case nvme_admin_create_sq:
        status = process_create_sq_command(&cmd->create_sq, &result_adm);
        break;
    default:
        status = NVME_SC_INVALID_OPCODE;
        break;
    }

    xil_printf("[Debug] process admin comand.\n");
    post_cqe(&nvme_queues[0], status, cmd->common.command_id, &result_adm);
    return status;
}

int parse_io_command(struct nvme_command* cmd, user_request* req)
{
    int status = 0;

    /* TODO: Support more namespaces. */
    if (cmd->rw.nsid != 1) {
        status = NVME_SC_INVALID_NS;
        return status;
    }

    memset(req, 0, sizeof(user_request));

    switch (cmd->rw.opcode) {
    case nvme_cmd_read:
        req->req_type = IOREQ_READ;
        break;
    case nvme_cmd_write:
        req->req_type = IOREQ_WRITE;
        break;
    case nvme_cmd_flush:
        req->req_type = IOREQ_FLUSH;
        break;
    }

    req->nsid = cmd->rw.nsid;
    req->req_id = cmd->rw.command_id;
    req->start_lba = cmd->rw.slba;
    req->sector_count = cmd->rw.length + 1;
    req->prps[0] = cmd->rw.dptr.prp1;
    req->prps[1] = cmd->rw.dptr.prp2;

    return status;
}

void get_nvme_command(int* last_request){
	nvme_queue* nvmeq;
	struct nvme_command* cmd = &submission_entry;

	nvmeq = get_queue(last_request);
	fetch_next_request(nvmeq, cmd);
	xil_printf("[Debug] fetch next request from queue %d\n", *last_request);
}
