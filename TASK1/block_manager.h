#ifndef _BLOCK_MAGER_
#define _BLOCK_MAGER_

#include <avl.h>
#include <config.h>
#include <kref.h>

typedef uint32_t mppn_t;
typedef uint32_t mvpn_t;
#define NO_MPPN UINT32_MAX


struct xlate_entry {
    ppa_t ppa;
};

struct xlate_page {
    mvpn_t mvpn;
    struct xlate_entry* entries;
    // int dirty;                       //whether the mapping information is modified,if TRUE will update mapping information to flash
    // mutex_t mutex;
    struct avl_node avl;
    // struct list_head lru;               //for replacement
    // unsigned int pin_count;
};

struct xlate_pcache {
    size_t capacity; 	// pre-defined size
    size_t size;		// real-time in use size
    struct avl_root root;
    // struct list_head lru_list;

    void* xlate_pages_addr;
};

typedef struct {
    struct kref kref;
	unsigned int nsid;
	size_t total_logical_pages;
	size_t used_pages;
	size_t gc_pages;
	enum plane_allocate_scheme pa_scheme;

	/* Global translation directory */
    mppn_t* gtd;
    size_t gtd_size;
    size_t xlate_ents_per_page;
    size_t total_xlate_pages;

    /* Translation page cache */
    struct xlate_pcache pcache;
} am_domain;

typedef struct {
	void* block_next;
	unsigned short block_id;
	unsigned int nr_invalid_pages;
	unsigned short page_write_index;
	unsigned int nsid;
	int flags;
	bitchunk_t invalid_page_bitmap[BITCHUNKS(PAGES_PER_BLOCK)];
} block_data;

typedef struct {
	block_data* free_blocks;
	block_data blocks[PAGES_PER_BLOCK];
	block_data* data_wf;
	block_data* gc_wf;
} plane_allocator;

void translate_ppa(flash_transaction* txn);
void domain_init(unsigned int nsid);
// void bm_init();

#endif
