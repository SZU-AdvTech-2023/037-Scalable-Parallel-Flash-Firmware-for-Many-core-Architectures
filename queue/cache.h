#ifndef _CACHE_H_
#define _CACHE_H_

#include <stdatomic.h>
#include <xil_types.h>

#include "config.h"
#include "request_types.h"

#define SYNC_ENABLE	1

typedef struct {
	atomic_flag _rwlock;
	int _readers;
	// lpa_t lpa;
	volatile unsigned int req_id;
	u8 data[FLASH_PG_SIZE];
} cacheline;

typedef struct {
	cacheline clines[CACHE_LINE_NUM];
} datacache;

void InitCache(datacache* dcache);
int GetRLock(datacache* dcache, unsigned int line);
void GetRLock_Blocked(datacache* dcache, unsigned int line);
void GetWLock(datacache* dcache, unsigned int line);
void ReleaseRLock(datacache* dcache, unsigned int line);
void ReleaseWLock(datacache* dcache, unsigned int line);
void* GetData(datacache* dcache, unsigned int line);
void SetData(datacache* dcache, unsigned int line, u8* data);
lpa_t GetTag(datacache* dcache, unsigned int line, lpa_t lpa);
void SetTag(datacache* dcache, unsigned int line, lpa_t lpa);

#endif
