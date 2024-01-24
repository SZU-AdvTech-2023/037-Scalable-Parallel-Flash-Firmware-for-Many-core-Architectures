#ifndef _CONFIG_H_
#define _CONFIG_H_

#define DEBUG

/*
* General configuration.
*/
#define PAGE_SIZE (16 << 10) // 16KB

/*
* Cache system configuration.
*/
#define CACHE_ENTRY_NUM (32 << 10) // 32K
#define CACHE_SWAP_NUM (2 << 10) // 2K >= the depth of Q3

#endif