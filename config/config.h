#ifndef _CONFIG_H_
#define _CONFIG_H_

#include "xparameters.h"
#include "xil_types.h"

#define ALIGNMENT_UP(addr, size)		((addr + size - 1) & ~(size - 1))

// the size of the mapping tablespace
#define CONFIG_MAPPING_TABLE_CAPACITY (320UL << 20)  /* 320MB */

/* Memory Zones */
#define MEMZONE_PS_DDR_HIGH 0
#define MEMZONE_PS_DDR_LOW  1
#define MEMZONE_PL_DDR      2
#define MEMZONE_MAX         3

/* Zone flags */
#define ZONE_PS_DDR_LOW  (1 << MEMZONE_PS_DDR_LOW)
#define ZONE_PS_DDR_HIGH (1 << MEMZONE_PS_DDR_HIGH)
#define ZONE_PL_DDR      (1 << MEMZONE_PL_DDR)
#define ZONE_DMA         (ZONE_PS_DDR_LOW | ZONE_PL_DDR)
#define ZONE_PS_DDR      (ZONE_PS_DDR_LOW | ZONE_PS_DDR_HIGH)
#define ZONE_ALL         (ZONE_PS_DDR_LOW | ZONE_PS_DDR_HIGH | ZONE_PL_DDR)

/* Request Queue */
#define TXN_POOL_LENGTH (32UL << 10)  /* 32K * 4KB = 128MB */


/* Addresses */
#define DDR_BASE_ADDR_0					XPAR_PSU_DDR_0_S_AXI_BASEADDR
#define DDR_BASE_ADDR_1 				XPAR_PSU_DDR_1_S_AXI_BASEADDR
#define MEM_BASE_ADDR_0 				(DDR_BASE_ADDR_0 + 0x10000000)
#define MEM_BASE_ADDR_1					DDR_BASE_ADDR_1

#define COREINF_SPACE_BASE				(MEM_BASE_ADDR_0 + 0) // 128B
#define NVMEQS_BASE						(COREINF_SPACE_BASE + 0x80) // 1496B
#define IDENTIFY_DATA_BASE				(ALIGNMENT_UP((NVMEQS_BASE + 0x5D8), 32)) // 4096B (32B alignment)

#define RQ_QUEUE_BASE					(IDENTIFY_DATA_BASE + 0x1000)
#define RQ_QUEUE_IN_USE					(RQ_QUEUE_BASE + 0x1000)
#define RQ_QUEUE_LOCK					(RQ_QUEUE_IN_USE + 0x1000)

#define CC_TX_BUFFER					(ALIGNMENT_UP((RQ_QUEUE_LOCK + 0x1000), 32))
#define DMA_BDRING_BUFFER				(ALIGNMENT_UP((CC_TX_BUFFER + 0x1000), 1024*1024)) //1MB (1MB alignment)

#define CC_AXI_DMA_BASE					(DMA_BDRING_BUFFER + 0x100000)
#define RQ_AXI_DMA_BASE					(CC_AXI_DMA_BASE + 0x1000)

#define RQ_AXI_DMA_LOCK					(RQ_AXI_DMA_BASE + 0x1000)

#define CC_RX_BUFFER_BASE				(ALIGNMENT_UP((RQ_AXI_DMA_LOCK + 0x1000), 32))

//#define TEMP_BUFFER						(ALIGNMENT_UP((CC_RX_BUFFER_BASE + 0x1000), 32))

#define TXN_POOL_BASE					(ALIGNMENT_UP(CC_RX_BUFFER_BASE + 0x100000, 32))

#define REQ_1_QUEUE_META_BASE			(TXN_POOL_BASE + 0x1000)
#define REQ_1_QUEUE_DATA_BASE			(REQ_1_QUEUE_META_BASE + 0x1000)

#define REQ_2_QUEUE_META_BASE			(MEM_BASE_ADDR_0 + 0)
#define REQ_2_QUEUE_DATA_BASE			(MEM_BASE_ADDR_0 + 0)

#define XLATE_PAGES_ADDR				(MEM_BASE_ADDR_0 + 0)

#define PLANES_BASE						(MEM_BASE_ADDR_0 + 0)
#define DCACHE_BASE						(MEM_BASE_ADDR_0 + 0)
//TODO

#define IOREQ_READ  					1
#define IOREQ_WRITE 					2
#define IOREQ_FLUSH 					3

#define CACHE_LINE_NUM 					(32 << 10)

typedef struct {
	volatile uint32_t pending_rqs;
	volatile uint32_t pending_rcs;
	volatile uint8_t rqs_runable;
	volatile uint8_t rcs_runable;
} core_inf;

typedef struct {
	core_inf cores[4];
} core_inf_array;

/*
 * HOST CONFIG
 */
#define PG_SHIFT 						12
#define PG_SIZE  						(1UL << PG_SHIFT)
#define SECTOR_SHIFT 					12
#define SECTOR_SIZE  					(1UL << SECTOR_SHIFT)
#define CONFIG_NVME_IO_QUEUE_MAX 		16
#define CONFIG_STORAGE_CAPACITY_BYTES 	(512ULL << 30) /* 512 GiB */
#define MAX_DATA_TRANSFER_SIZE 			8 /* 256 pages */
/*
 * FLASH CONFIG
 */
#define FLASH_PG_SHIFT    14U
#define FLASH_PG_SIZE     				(16384)
#define FLASH_PG_OOB_SIZE 				(1872)
#define FLASH_PG_BUFFER_SIZE 			(FLASH_PG_SIZE + FLASH_PG_OOB_SIZE)

#define SECTORS_PER_FLASH_PG 			(FLASH_PG_SIZE >> SECTOR_SHIFT)

#define CHIPS_ENABLED_PER_CHANNEL CHIPS_PER_CHANNEL

#define NR_CHANNELS       				8
#define CHIPS_PER_CHANNEL 				2
#define DIES_PER_CHIP     				2
#define PLANES_PER_DIE    				2
#define BLOCKS_PER_PLANE  				1048
#define PAGES_PER_BLOCK   				512

#define PLANE_INFO_FILE 				"planes.bin"
#define BAD_BLOCKS_FILE 				"badblks.bin"

#define BF_BAD     						0x1
#define BF_MAPPING 						0x2

#define DEFAULT_PLANE_ALLOCATE_SCHEME PAS_CWDP


/* #define ENABLE_MULTIPLANE */

/* clang-format off */

#define CE_PINS \
    30, 27, \
    37, 43, \
    31, 21, \
    38, 54, \
    67, 64, \
    60, 66, \
    56, 59, \
    55, 75,

/* clang-format on */

#define WP_PINS 29, 26, 52, 53, 14, 24, 62, 68

#define BCH_BLOCK_SIZE 					(512)
#define BCH_CODE_SIZE  					(20)

#endif
