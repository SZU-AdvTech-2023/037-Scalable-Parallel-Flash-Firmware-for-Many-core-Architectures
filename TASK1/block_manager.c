#include "stdlib.h"
#include "string.h"

#include "config.h"
#include "request_types.h"
#include "block_manager.h"
#include <errno.h>

#include "bitmap.h"
#include "slab.h"
#include "stdio.h"
#include "ff.h"

#define NAME "[AMU]"
// The first 40 blocks are mapping blocks
#define START_PPA 512*40
#define GTD_FILENAME "gtd_ns%d.bin"

#define PAGES_PER_PLANE   (PAGES_PER_BLOCK * BLOCKS_PER_PLANE)
#define PAGES_PER_DIE     (PAGES_PER_PLANE * PLANES_PER_DIE)
#define PAGES_PER_CHIP    (PAGES_PER_DIE * DIES_PER_CHIP)
#define PAGES_PER_CHANNEL (PAGES_PER_CHIP * CHIPS_ENABLED_PER_CHANNEL)


static plane_allocator**** planes;
static am_domain g_domain;
static FATFS fatfs;

// the size of the transltate_page
#define XLATE_PG_SIZE	FLASH_PG_SIZE


extern struct channel_data channel_data[NR_CHANNELS];

static inline am_domain* domain_get(unsigned int nsid){
	Xil_AssertNonvoid(nsid == 1);
	return &g_domain;
}

static inline void assign_plane(am_domain* domain, flash_address* addr, lpa_t lpa){
	static const unsigned int channel_count = NR_CHANNELS;
    static const unsigned int chip_count = CHIPS_ENABLED_PER_CHANNEL;
    static const unsigned int die_count = DIES_PER_CHIP;
    static const unsigned int plane_count = PLANES_PER_DIE;

#define ASSIGN_PHYS_ADDR(lpa, name)			\
	do {									\
		addr->name = (lpa) % name##_count; 	\
		(lpa) = (lpa) / name##_count;		\
	} while (0)
#define ASSIGN_PLANE(lpa, first, second, third, fourth)	\
	do {												\
		ASSIGN_PHYS_ADDR(lpa, first);					\
		ASSIGN_PHYS_ADDR(lpa, second);					\
		ASSIGN_PHYS_ADDR(lpa, third);					\
		ASSIGN_PHYS_ADDR(lpa, fourth);					\
	} while (0)

	switch (domain->pa_scheme) {
    case PAS_CWDP:
        ASSIGN_PLANE(lpa, channel, chip, die, plane);
        break;
    case PAS_CWPD:
        ASSIGN_PLANE(lpa, channel, chip, plane, die);
        break;
    case PAS_CDWP:
        ASSIGN_PLANE(lpa, channel, die, chip, plane);
        break;
    case PAS_CDPW:
        ASSIGN_PLANE(lpa, channel, die, plane, chip);
        break;
    case PAS_CPWD:
        ASSIGN_PLANE(lpa, channel, plane, chip, die);
        break;
    case PAS_CPDW:
        ASSIGN_PLANE(lpa, channel, plane, die, chip);
        break;
    case PAS_WCDP:
        ASSIGN_PLANE(lpa, chip, channel, die, plane);
        break;
    case PAS_WCPD:
        ASSIGN_PLANE(lpa, chip, channel, plane, die);
        break;
    case PAS_WDCP:
        ASSIGN_PLANE(lpa, chip, die, channel, plane);
        break;
    case PAS_WDPC:
        ASSIGN_PLANE(lpa, chip, die, plane, channel);
        break;
    case PAS_WPCD:
        ASSIGN_PLANE(lpa, chip, plane, channel, die);
        break;
    case PAS_WPDC:
        ASSIGN_PLANE(lpa, chip, plane, die, channel);
        break;
    case PAS_DCWP:
        ASSIGN_PLANE(lpa, die, channel, chip, plane);
        break;
    case PAS_DCPW:
        ASSIGN_PLANE(lpa, die, channel, plane, chip);
        break;
    case PAS_DWCP:
        ASSIGN_PLANE(lpa, die, chip, channel, plane);
        break;
    case PAS_DWPC:
        ASSIGN_PLANE(lpa, die, chip, plane, channel);
        break;
    case PAS_DPCW:
        ASSIGN_PLANE(lpa, die, plane, channel, chip);
        break;
    case PAS_DPWC:
        ASSIGN_PLANE(lpa, die, plane, chip, channel);
        break;
    case PAS_PCWD:
        ASSIGN_PLANE(lpa, plane, channel, chip, die);
        break;
    case PAS_PCDW:
        ASSIGN_PLANE(lpa, plane, channel, die, chip);
        break;
    case PAS_PWCD:
        ASSIGN_PLANE(lpa, plane, chip, channel, die);
        break;
    case PAS_PWDC:
        ASSIGN_PLANE(lpa, plane, chip, die, channel);
        break;
    case PAS_PDCW:
        ASSIGN_PLANE(lpa, plane, die, channel, chip);
        break;
    case PAS_PDWC:
        ASSIGN_PLANE(lpa, plane, die, chip, channel);
        break;
	}

#undef ASSIGN_PLANE
#undef ASSIGN_PHYS_ADDR
}

static struct xlate_page* xpc_find(struct xlate_pcache* xpc, mvpn_t mvpn)
{
    struct avl_node* node = xpc->root.node;
    struct xlate_page* xpg = NULL;

    while (node) {
        xpg = avl_entry(node, struct xlate_page, avl);

        if (xpg->mvpn == mvpn) {
            return xpg;
        } else if (mvpn < xpg->mvpn)
            node = node->left;
        else if (mvpn > xpg->mvpn)
            node = node->right;
    }

    return NULL;
}

static int xpc_add(struct xlate_pcache* xpc, mvpn_t mvpn,
                   struct xlate_page** xpgpp)
{
    struct xlate_page* xpg;

    if (xpc->size >= xpc->capacity) return ENOSPC;

    SLABALLOC(xpg);
    if (!xpc) return ENOMEM;

    memset(xpg, 0, sizeof(*xpg));
    xpg->mvpn = mvpn;

    /* Allocate buffer for the translation page. Prefer PS DDR. */
    xpg->entries = xpc->xlate_pages_addr + mvpn * XLATE_PG_SIZE;
    if (!xpg->entries) {
        SLABFREE(xpg);
        return ENOMEM;
    }

    avl_insert(&xpg->avl, &xpc->root);
    xpc->size++;
    *xpgpp = xpg;

    return 0;
}

static int xpc_read_page(am_domain* domain, struct xlate_page* xpg) // TODO
{
    mvpn_t mvpn = xpg->mvpn;

        int i;
        for (i = 0; i < domain->xlate_ents_per_page; i++) {
            xpg->entries[i].ppa = mvpn * domain->xlate_ents_per_page + i + START_PPA;
            xpg->entries[i].bitmap = (1UL << SECTORS_PER_FLASH_PG) - 1;
        }
    
    return 0;
}

/* xpg
--------------------				------------
|		mvpn		|				|	ppa0	|
--------------------			\	------------
|		entries		| ----------	|	...		|
--------------------			/	------------
|		avl			|				|	ppa4095	|
--------------------				------------
 */


/* Get the translation page referenced by mvpn from the page cache. Return
 * the page exclusively locked and detached from LRU. */
static int get_translation_page(am_domain* domain, mvpn_t mvpn, struct xlate_page** xpgpp)
{
    struct xlate_page* xpg;
    struct xlate_pcache* xpc = &domain->pcache;
    int r = 0;

    xpg = xpc_find(xpc, mvpn);

	if(!xpg){
        /* Try to add a new translation page. */
        r = xpc_add(xpc, mvpn, &xpg);
		if (unlikely(r != 0)) {
            /* Error. */
            return r;
        } else {
            r = xpc_read_page(domain, xpg); 
        }
    }

    *xpgpp = xpg;
    return r;
}

static inline mvpn_t get_mvpn(am_domain* domain, lpa_t lpa)
{
    return lpa / domain->xlate_ents_per_page;
}

/* Get the slot within a mapping virtual page for an LPA. */
static inline unsigned int get_mvpn_slot(am_domain* domain, lpa_t lpa)
{
    return lpa % domain->xlate_ents_per_page;
}

static int get_ppa(am_domain* domain, lpa_t lpa, ppa_t* ppap, struct xlate_page* xpg)
{
    mvpn_t mvpn = get_mvpn(domain, lpa);
    unsigned int slot = get_mvpn_slot(domain, lpa);
    int r;

    r = get_translation_page(domain, mvpn, &xpg);
    if (r) return r;

    *ppap = xpg->entries[slot].ppa;

    return 0;
}

static inline void update_ppa(am_domain* domain, lpa_t lpa, ppa_t ppa,struct xlate_page* xpg){
	mvpn_t mvpn = get_mvpn(domain,lpa);
	unsigned int slot = get_mvpn_slot(domain, lpa);
	xpg->entries[slot].ppa = ppa;
}

static inline void ppa_to_address(ppa_t ppa, flash_address* addr){
#define XLATE_PPA(ppa, name, cname)           \
    do {                                      \
        addr->name = ppa / PAGES_PER_##cname; \
        ppa = ppa % PAGES_PER_##cname;        \
    } while (0)
    XLATE_PPA(ppa, channel, CHANNEL);
    XLATE_PPA(ppa, chip, CHIP);
    XLATE_PPA(ppa, die, DIE);
    XLATE_PPA(ppa, plane, PLANE);
    XLATE_PPA(ppa, block, BLOCK);
    addr->page = ppa;
#undef XLATE_PPA
}

static inline ppa_t address_to_ppa(flash_address* addr){
	ppa_t ppa = 0;
#define XLATE_ADDR(addr, name, cname)			\
	do {										\
		ppa += addr->name * PAGES_PER_##cname;	\
	} while (0)
	XLATE_ADDR(addr, channel, CHANNEL);
	XLATE_ADDR(addr, chip, CHIP);
	XLATE_ADDR(addr, die, DIE);
	XLATE_ADDR(addr, plane, PLANE);
	XLATE_ADDR(addr, block, BLOCK);
	ppa += addr->page;
#undef XLATE_ADDR
}

static inline plane_allocator* get_plane(flash_address* addr){
	Xil_AssertNonvoid(addr->channel < NR_CHANNELS);
	Xil_AssertNonvoid(addr->chip < CHIPS_PER_CHANNEL);
	Xil_AssertNonvoid(addr->die < DIES_PER_CHIP);
	Xil_AssertNonvoid(addr->plane < PLANES_PER_DIE);
	return &planes[addr->channel][addr->chip][addr->die][addr->plane];
}

static inline block_data* get_block_data(plane_allocator* plane, unsigned int block_id){
	return &plane->blocks[block_id];
}

static void invalidate_page(flash_address* addr){
	plane_allocator* plane = get_plane(addr);
	block_data* block = get_block_data(plane, addr->block);

	block->nr_invalid_pages++;
	SET_BIT(block->invalid_page_bitmap, addr->page);
}

static void update_gc_status(am_domain* domain, ppa_t ppa, flash_address* addr){
	ppa_to_address(ppa, addr);
	invalidate_page(addr);
}

static block_data* get_free_block(plane_allocator* plane){
	block_data* block;

	if (plane->free_blocks == NULL){
		return NULL;
	}
	block = plane->free_blocks;
	plane->free_blocks = block->block_next;

	return block;
}

static void allocate_page(flash_address* addr, int for_gc){
	block_data* block;

	block = for_gc ? plane->gc_wf : plane->data_wf;
	addr->block = block->block_id;
	addr->page = block->page_write_index++;
	if (block->page_write_index == PAGES_PER_BLOCK){
		block = get_free_block(plane);
		if (for_gc){
			plane->gc_wf = block;
		} else {
			plane->data_wf = block;
		}
	}
	//TODO: GC if block is NULL
}

void translate_ppa(flash_transaction* txn){
	flash_address addr;

	am_domain* domain = domain_get(txn->nsid);
    struct xlate_page* xpg;
	ppa_t ppa, evict_ppa;

	//read && miss
	if(txn->type == TXN_READ && txn->hit == 0){
		get_ppa(domain, txn->lpa,&ppa,NULL,&xpg);
	}
				
	if (txn->evict_lpa != NO_LPA){
        // get evict_ppa_old
		get_ppa(domain, txn->evict_lpa,&evict_ppa,NULL,&xpg);

		if (evict_ppa != NO_PPA){
			update_gc_status(domain, evict_ppa,&addr);
		}
		
		txn->evict_ppa_old = evict_ppa;
		assign_plane(domain,&addr,txn);
		allocate_page(&addr, 0);
        // get evict_ppa_new
		txn->evict_ppa = address_to_ppa(&addr);
        // update evict_lpa mapping  evict_ppa_new
		update_ppa(domain, txn->evict_lpa, txn->evict_ppa,&xpg);
	} else {
		txn->evict_ppa = NO_PPA;
	}
	txn->ppa = ppa;
}

static int xpc_key_node_comp(void* key, struct avl_node* node)
{
    struct xlate_page* r1 = (struct xlate_page*)key;
    struct xlate_page* r2 = avl_entry(node, struct xlate_page, avl);

    if (r1->mvpn < r2->mvpn)
        return -1;
    else if (r1->mvpn > r2->mvpn)
        return 1;
    return 0;
}

static int xpc_node_node_comp(struct avl_node* node1, struct avl_node* node2)
{
    struct xlate_page* r1 = avl_entry(node1, struct xlate_page, avl);
    struct xlate_page* r2 = avl_entry(node2, struct xlate_page, avl);

    if (r1->mvpn < r2->mvpn)
        return -1;
    else if (r1->mvpn > r2->mvpn)
        return 1;
    return 0;
}

static void xpc_init(struct xlate_pcache* xpc, size_t capacity)
{
    xpc->capacity = capacity;
    xpc->size = 0;
    INIT_AVL_ROOT(&xpc->root, xpc_key_node_comp, xpc_node_node_comp);
    xpc->xlate_pages_addr = XLATE_PAGES_ADDR;
}

static inline int init_emmc(void) { return f_mount(&fatfs, "", 0); }

static int save_mapping_entries (am_domain* domain, const char* filename) {
	FIL fil;
	size_t write_size;
	unsigned int br;
	int rc;

	write_size = domain->total_xlate_pages * XLATE_PG_SIZE;
	rc = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
	if (rc) return EIO;

	rc = f_write(&fil, domain->pcache->xlate_pages_addr, write_size, &bw);
	if (rc || bw != write_size) return EIO;

	rc = f_close(&fil);
	return rc > 0 ? EIO : 0;
}

static int  restore_mapping_entries (am_domain* domain, const char* filename) {
	FIL fil;
	size_t read_size;
	unsigned int br;
	int rc;

	read_size = domain->total_xlate_pages * XLATE_PG_SIZE;
	rc = f_open(&fil, filename, FA_READ);
	if (rc) return EIO;

	rc = f_read(&fil, domain->pcache->xlate_pages_addr, read_size, &br);
	if (rc || br != read_size) return EIO;

	rc = f_close(&fil);
	return rc > 0 ? EIO : 0;
}

static void domain_init(unsigned int nsid)
{
    char gtd_filename[20];
	am_domain* domain = domain_get(nsid);
    size_t gtd_size;
	size_t total_logical_pages = CONFIG_STORAGE_CAPACITY_BYTES >> FLASH_PG_SHIFT; 
	// the total number of xpg that can be stored
	size_t capacity = CONFIG_MAPPING_TABLE_CAPACITY / XLATE_PG_SIZE;
	// the number of ppa in translate page
    size_t xlate_ents_per_page = XLATE_PG_SIZE / sizeof(struct xlate_entry);
	// The total number of translation pages that needed
    size_t total_xlate_pages = total_logical_pages / xlate_ents_per_page;
    FILINFO fno;
    int i, r = 0;

    gtd_size = total_xlate_pages * sizeof(mppn_t);
    gtd_size = roundup(gtd_size, PG_SIZE);

    memset(domain, 0, sizeof(*domain));
    kref_init(&domain->kref);
    domain->nsid = nsid;
    domain->total_logical_pages = total_logical_pages;
    domain->xlate_ents_per_page = xlate_ents_per_page;
    domain->total_xlate_pages = total_xlate_pages;
    domain->pa_scheme = DEFAULT_PLANE_ALLOCATE_SCHEME;

    xpc_init(&domain->pcache, capacity);

    // TODO: init memory space.

    domain->gtd = alloc_pages(gtd_size >> PG_SHIFT, ZONE_PS_DDR);
    if (!domain->gtd) goto fail_free_xpc;
    domain->gtd_size = gtd_size;

    snprintf(gtd_filename, sizeof(gtd_filename), GTD_FILENAME, domain->nsid);

    if (f_stat(gtd_filename, &fno) != 0) {
        xil_printf(
            NAME
            " Initializing new global translation directory for namespace %d ...",
            nsid);

        for (i = 0; i < total_xlate_pages; i++) {
            domain->gtd[i] = NO_MPPN;
        }

        r = save_gtd(domain, gtd_filename);
        if (r) {
            r = EIO;
        } else {
            xil_printf("OK\n");
        }
    } else {
        xil_printf(
            NAME
            " Restoring global translation directory for namespace %d (%lu bytes) ...",
            nsid, fno.fsize);
        r = restore_gtd(domain, gtd_filename);
        if (r) {
            r = EIO;
        } else {
            xil_printf("OK\n");
        }
    }

    if (r != 0) {
        xil_printf("FAILED (%d)\n", r);
        goto fail_free_gtd;
    }

    xil_printf(NAME " Initialized namespace %d with %lu logical pages\n",nsid,
               total_logical_pages);

fail_free_gtd:
    free_mem(__pa(domain->gtd), domain->gtd_size);
fail_free_xpc:
    xpc_free(&domain->pcache);
}

static void init_plane(plane_allocator* plane){
	plane->free_blocks = NULL;
	for (int i = 0; i < BLOCKS_PER_PLANE; i++) {
		block_data* block = get_block_data(plane, i);
		block->nr_invalid_pages = 0;
		block->block_id = i;
		block->page_write_index = 0;
		block->flags = 0;
		block->block_next = NULL;
		memset(&block->invalid_page_bitmap, 0, sizeof(block->invalid_page_bitmap));
	}
}

static void init_plane_wf(plane_allocator* plane){
	plane->data_wf = get_free_block(plane);
	plane->gc_wf = get_free_block(plane);
}

static void reset_blocks(plane_allocator* plane){
	block_data* pre_block = plane->free_blocks;

	for (int i = 0; i < BLOCKS_PER_PLANE; i++) {
		block_data* block = get_block_data(plane, i);
		pre_block->block_next = block;
		pre_block = block;
	}
}

static void reset_planes(){
    for (int i = 0; i < NR_CHANNELS; i++) {
        for (int j = 0; j < CHIPS_PER_CHANNEL; j++) {
            for (int k = 0; k < DIES_PER_CHIP; k++) {
                for (int l = 0; l < PLANES_PER_DIE; l++) {
                    reset_blocks(&planes[i][j][k][l]);
                }
            }
        }
    }
}
