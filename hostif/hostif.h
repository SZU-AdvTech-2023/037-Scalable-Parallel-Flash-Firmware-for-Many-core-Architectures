#ifndef _HOSTIF_H_
#define _HOSTIF_H_

#include "request_types.h"
#include "iov_iter.h"
#include "xil_types.h"

typedef struct {
    u16 qid;

    u16 sq_depth;
    u16 cq_depth;
    u64 sq_dma_addr;
    u64 cq_dma_addr;

    u16 sq_head;
    u16 sq_tail;
    u16 cq_head;
    u16 cq_tail;
    uint8_t cq_phase;

    u16 cq_vector;
} nvme_queue;

extern nvme_queue* nvme_queues;
extern struct nvme_command submission_entry __attribute__((aligned(64)));
extern struct nvme_completion completion_entry __attribute__((aligned(64)));
extern union nvme_result result_adm;

/* nvme.c */
void nvme_init();
int nvme_dma_read(user_request* req, iov_iter* iter, size_t count);
int nvme_dma_write(user_request* req, iov_iter* iter, size_t count);
int write_prp_data(iov_iter* iter, u64 prp1, u64 prp2, size_t count);
void nvme_handle_reg_read(unsigned long addr, u16 requester_id, u8 tag, size_t len);
void nvme_handle_reg_write(unsigned long addr, const u8* buf, size_t len);
void get_nvme_command(int* last_request);
int process_admin_command(struct nvme_command* cmd);
int process_io_command(struct nvme_command* cmd, user_request* req);
int post_cqe(nvme_queue* nvmeq, int status, uint16_t command_id, union nvme_result* result);

/* nvme_identify.c */
void nvme_identify_namespace(u32 nsid, u8* data);
void nvme_identify_controller(u8* data);
void nvme_identify_ns_active_list(u8* data);
void nvme_identify_cs_controller(u8 csi, u8* data);

/* pcie.c */
int pcie_setup();
void pcie_stop();
int semi_pcie_setup();
int pcie_dma_read_iter(unsigned long addr, iov_iter* iter, size_t count);
int pcie_dma_write_iter(unsigned long addr, iov_iter* iter, size_t count);
int pcie_dma_read(unsigned long addr, u8* buffer, size_t count);
int pcie_dma_write(unsigned long addr, const u8* buffer, size_t count);
int pcie_send_completion(unsigned long addr, u16 requester_id, u8 tag, const u8* buffer, size_t count);
int pcie_send_msi(u16 vector);


#endif
