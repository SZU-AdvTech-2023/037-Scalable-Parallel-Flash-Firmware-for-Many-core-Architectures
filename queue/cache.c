#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "config.h"
#include "request_types.h"
#include "utils.h"


static void InitCacheline(cacheline* cline){
	atomic_flag_clear(&cline->_rwlock);
	cline->_readers = 0;
}

void InitCache(datacache* dcache){
	for(int i = 0; i < CACHE_LINE_NUM; i++){
		InitCacheline(&dcache->clines[i]);
	}
}

int GetRLock(datacache* dcache, unsigned int line){
	cacheline* cline = &dcache->clines[line];
	atomic_fetch_add(&cline->_readers, 1);
	if (!(cline->_rwlock.__val)) return 0;
	atomic_fetch_sub(&cline->_readers, 1);
	return -1;
}

void GetRLock_Blocked(datacache* dcache, unsigned int line){
	cacheline* cline = &dcache->clines[line];
	atomic_fetch_add(&cline->_readers, 1);
	while(cline->_rwlock.__val);
}

void GetWLock(datacache* dcache, unsigned int line){
	cacheline* cline = &dcache->clines[line];

	while(atomic_flag_test_and_set(&cline->_rwlock));
	while(cline->_readers);
}

void ReleaseRLock(datacache* dcache, unsigned int line){
	cacheline* cline = &dcache->clines[line];
	atomic_fetch_sub(&cline->_readers, 1);
}

void ReleaseWLock(datacache* dcache, unsigned int line){
	cacheline* cline = &dcache->clines[line];
	atomic_flag_clear(&cline->_rwlock);
}

void* GetData(datacache* dcache, unsigned int line){
	cacheline* cline = &dcache->clines[line];
	return (void*)cline->data;
}

void SetData(datacache* dcache, unsigned int line, u8* data){
	cacheline* cline = &dcache->clines[line];
	memcpy((void*)cline->data, (void*)data, FLASH_PG_SIZE);
}

lpa_t GetTag(datacache* dcache, unsigned int line, unsigned int req_id){
	cacheline* cline = &dcache->clines[line];
	return cline->req_id;
}

void SetTag(datacache* dcache, unsigned int line, unsigned int req_id){
	cacheline* cline = &dcache->clines[line];
	cline->req_id = req_id;
#ifdef SYNC_ENABLE
	smp_mb();
#endif
}
