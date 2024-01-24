#ifndef _DATACACHE_
#define _DATACACHE_

#include <stdint.h>
#include "rwlock.h"
#include "config.h"


enum cache_entry_status{
    CES_EMPTY,
    CES_CLEAN,
    CES_DIRTY,
};

typedef struct Page{
    char content[PAGE_SIZE];
} page;

typedef struct CacheEntry{
    enum cache_entry_status status;
    uint64_t tag; // In this version, the tag is lpa
    uint64_t bitmap;
    rwlock lock;
    page* data;
} cache_entry;

typedef struct DataCache{
    uint32_t init_flag;
    uint32_t swap_pages_index;
    uint64_t tags[CACHE_ENTRY_NUM]; // tag' used by Task0
    cache_entry cachelines[CACHE_ENTRY_NUM];
    page* swap_pages_addr[CACHE_SWAP_NUM]; // Store the addr of 
    page pages[CACHE_ENTRY_NUM + CACHE_SWAP_NUM];
} data_cache;

int DataCache_init(data_cache* cache);

uint32_t GetLineNum(uint64_t lpa);

// Only Task0 call this function.
int CheckDataCacheTag(data_cache* cache, uint32_t line_num, uint64_t lpa);

// Only Task0 call this function.
int Check_UpdateDataCacheTag(data_cache* cache, uint32_t line_num, uint64_t lpa);

void R_LockCacheEntry(data_cache* cache, uint32_t line_num);

void R_UnlockCacheEntry(data_cache* cache, uint32_t line_num);

void W_LockCacheEntry(data_cache* cache, uint32_t line_num);

void W_UnlockCacheEntry(data_cache* cache, uint32_t line_num);

// Only Task2 call this function. No lock
page* GetDataBuffer(data_cache* cache);

// Only Task3 call this function. R_Lock
page* GetDataFromCache(data_cache* cache, uint32_t line_num); // Hit

/* Only Task3 call this function. W_Lock
* Update the new data stored in the swap_pages_addr[swap_page_index] to the cachelines[line_num]. 
*/
void UpdateDataToCache(data_cache* cache, uint32_t swap_page_index, uint32_t line_num, uint64_t lpa); // Miss

#endif