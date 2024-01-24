#ifndef _REQ_TYPES_H_
#define _REQ_TYPES_H_

#include <stdint.h>
#include "config.h"
#include "list.h"

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;

typedef uint64_t lha_t; /* logical host address */
typedef uint32_t pda_t; /* physical device address */
typedef uint64_t page_bitmap_t;

typedef uint64_t lpa_t;
/* 4G PPA * 16k page = 64 TB max. size */
typedef uint32_t ppa_t;
#define NO_LPA UINT64_MAX
#define NO_PPA UINT32_MAX

#define MISS_NONDIRTY 0
#define MISS_DIRTY 1
#define HIT 2

typedef struct {
    unsigned int total_flash_read_txns;
    unsigned int total_flash_write_txns;
    unsigned int total_flash_read_bytes;
    unsigned int total_flash_write_bytes;
    unsigned int ecc_error_blocks;
    unsigned int flash_read_transfer_us;
    unsigned int flash_write_transfer_us;
    unsigned int flash_read_command_us;
    unsigned int flash_write_command_us;
} user_request_stats;

typedef struct {
    int req_type;
    int nvme_queue_id;
    unsigned int nsid;
    unsigned int req_id;
    lha_t start_lba;
    unsigned int sector_count;
    unsigned int status;
    uint64_t prps[2];

    unsigned int txn_length;

    struct list_head txn_head;

    user_request_stats stats;
} user_request;

typedef struct {
    unsigned int channel;
    unsigned int chip;
    unsigned int die;
    unsigned int plane;
    unsigned int block;
    unsigned int page;
} flash_address;

/* Channel-Way-Die-Plane */
enum plane_allocate_scheme {
    PAS_CWDP,
    PAS_CWPD,
    PAS_CDWP,
    PAS_CDPW,
    PAS_CPWD,
    PAS_CPDW,
    PAS_WCDP,
    PAS_WCPD,
    PAS_WDCP,
    PAS_WDPC,
    PAS_WPCD,
    PAS_WPDC,
    PAS_DCWP,
    PAS_DCPW,
    PAS_DWCP,
    PAS_DWPC,
    PAS_DPCW,
    PAS_DPWC,
    PAS_PCWD,
    PAS_PCDW,
    PAS_PWCD,
    PAS_PWDC,
    PAS_PDCW,
    PAS_PDWC,
};

enum txn_type {
    TXN_READ,
    TXN_WRITE,
    TXN_ERASE,
	TXN_FLUSH,
};

enum txn_source {
    TS_USER_IO,
    TS_MAPPING,
    TS_GC,
};

enum nvme_cmd_type {
	ADMIN_CMD,
	IO_CMD,
};

struct flash_address {
    unsigned int channel;
    unsigned int chip;
    unsigned int die;
    unsigned int plane;
    unsigned int block;
    unsigned int page;
};

typedef struct {
    unsigned char* data; // point to the txn_pool's data buffer
    unsigned char* victim_data;	// point to the cacheline

	enum nvme_cmd_type cmd_type;	// 0-admin 1-io
    enum txn_type type; // read write earse
    enum txn_source source; // user gc mapping
	int nvmeq_num;	// one of the 16 nvme queue
	int Status;
    unsigned int req_id;
    unsigned int req_id_victim;
#ifdef _NVME_H_
	union nvme_result result_adm;
#else
	union nvme_result {
		__le16 u16;
		__le32 u32;
		__le64 u64;
	} result_adm;
#endif
	uint64_t prps[2];

	unsigned char hit; // 0-miss&non-dirty 1-miss&dirty 2-hit
	unsigned char fil_task_level;
	struct flash_address fil_inuse_addr;

	lpa_t evict_lpa;
	ppa_t evict_ppa;
    ppa_t evict_ppa_old; 

    unsigned int nsid;
    lpa_t lpa;	// page-level logical address
    ppa_t ppa;
    int ppa_ready;
    void* opaque;
    unsigned int ecc_status;

    unsigned int offset; // unit of byte
    unsigned int length; // unit of byte

    uint8_t* code_buf; //txn->code_buf = txn->data + FLASH_PG_SIZE;
    unsigned int code_length;

    int completed;

    struct list_head txn_list_handle; // link_list handle

    unsigned int txn_num; // sequence number in the segmented request
	unsigned int txn_length; // total number of the segmented request

    uint32_t total_xfer_us;
    uint32_t total_exec_us;
    uint64_t err_bitmap;
} flash_transaction;

#endif
