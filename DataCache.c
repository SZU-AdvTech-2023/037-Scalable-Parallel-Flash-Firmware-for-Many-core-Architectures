#include "DataCache.h"

int DataCache_init(data_cache* cache){
    for(int i = 0; i < CACHE_SWAP_NUM; i++){
        cache->swap_pages_addr[i] = &(cache->pages[i]);
    }
    for(int i = 0; i < CACHE_ENTRY_NUM; i++){
        cache->cachelines[i].data = &(cache->pages[CACHE_SWAP_NUM+i]);
        cache->cachelines[i].status = CES_EMPTY;
    }
    cache->swap_pages_index = 0;
    cache->init_flag = 1;
    return 0;
}

uint32_t GetLineNum(uint64_t lpa){
    return lpa%CACHE_ENTRY_NUM;
}

int CheckDataCache(data_cache* cache, uint32_t line_num, uint64_t lpa){
    int result = 0;
    if(cache->cachelines[line_num].tag == lpa) result = 1;
    return result;
}

int Check_UpdateDataCacheTag(data_cache* cache, uint32_t line_num, uint64_t lpa){
    int result = 0;
    if(cache->tags[line_num] == lpa){
        result = 1;
    }else{
        cache->tags[line_num] = lpa;
    }
    return result;
}

void R_LockCacheEntry(data_cache* cache, uint32_t line_num){
    lock_r(&(cache->cachelines[line_num].lock));
}

void R_UnlockCacheEntry(data_cache* cache, uint32_t line_num){
    unlock_r(&(cache->cachelines[line_num].lock));
}

void W_LockCacheEntry(data_cache* cache, uint32_t line_num){
    lock_w(&(cache->cachelines[line_num].lock));
}

void W_UnlockCacheEntry(data_cache* cache, uint32_t line_num){
    unlock_w(&(cache->cachelines[line_num].lock));
}

page* GetDataBuffer(data_cache* cache){
    uint32_t buf_index = cache->swap_pages_index;
    page* buf= cache->swap_pages_addr[buf_index];
    cache->swap_pages_index++;
    if(cache->swap_pages_index == CACHE_SWAP_NUM) cache->swap_pages_index = 0;
    return buf;
}

page* GetDataFromCache(data_cache* cache, uint32_t line_num){
    cache_entry *cacheline = cache->cachelines + line_num;
    return cacheline->data;
}

void UpdateDataToCache(data_cache* cache, uint32_t swap_page_index, uint32_t line_num, uint64_t lpa){
    cache_entry *cacheline = cache->cachelines + line_num;
    page* newdata = cache->swap_pages_addr[swap_page_index];
    page* olddata = cacheline->data;
    cache->swap_pages_addr[swap_page_index] = olddata;
    cacheline->data = newdata;
    cacheline->tag = lpa;
}
