/* Flash Interface Layer */

#include "xdebug.h"
#include "xil_assert.h"
#include "xil_mmu.h"
#include "xparameters.h"
#include "xgpiops.h"
#include <sleep.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "request_types.h"
#include "bch_engine.h"
#include "fil.h"
#include "flash.h"
#include "nano_controller.h"
#include "config.h"
#include "utils.h"


#define INPUT_DELAY_MAX  512
#define INPUT_DELAY_STEP 2

#define TRAINING_BLOCK 12
#define TRAINING_PAGE  1

struct chip_data;
struct channel_data;


enum die_status {
	DS_IDLE = 0,
	DS_CMD_IN,
	DS_CMD_EXE,
	DS_DATA_IN,
	DS_DATA_OUT,
};

struct flash_txn_param {
	enum txn_type type;
	struct flash_address addr;
    unsigned int offset;
    unsigned int length;
    unsigned char* data_buffer;
};

struct die_data {
    unsigned int index;

    enum die_status status;

    unsigned char buffer[FLASH_PG_BUFFER_SIZE];
    unsigned char* code_buffer = buffer + FLASH_PG_SIZE;

    struct chip_data* chip;

    struct flash_transaction* active_txn;

    u64 exec_start_cycle;
};

struct chip_data {
    int index;

    int ce_pin;
    u8 ce_gpio_bank;
    u8 ce_gpio_pin;

    enum chip_status status;

    struct channel_data* channel;
    struct die_data dies[DIES_PER_CHIP];

    struct list_head txn_queue;
    int current_active_die;
};

enum channel_status {
    BUS_IDLE = 0,
    BUS_BUSY,
};

struct channel_data {
    int index;

    enum channel_status status;

    struct nf_controller nfc;

    struct chip_data chips[CHIPS_PER_CHANNEL];
};

static struct channel_data channel_data[NR_CHANNELS];

unsigned char* RMW_buffer; // 32-byte aligned

static int ce_pins[] = {CE_PINS};
static int wp_pins[] = {WP_PINS};

static XGpioPs ps_gpio_inst;

struct list_head finish_queue;
struct list_head wait_queue;

static struct nfc_config {
    uintptr_t base_addr;
    int dma_dev_id;
    unsigned int odt_config[CHIPS_PER_CHANNEL];
} nfc_configs[] = {
    {XPAR_ONFI_BCH_PHY_0_BASEADDR, XPAR_AXIDMA_2_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_PHY_1_BASEADDR, XPAR_AXIDMA_3_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_PHY_2_BASEADDR, XPAR_AXIDMA_4_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_PHY_3_BASEADDR, XPAR_AXIDMA_5_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_PHY_4_BASEADDR, XPAR_AXIDMA_6_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_PHY_5_BASEADDR, XPAR_AXIDMA_7_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_PHY_6_BASEADDR, XPAR_AXIDMA_8_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_PHY_7_BASEADDR, XPAR_AXIDMA_9_DEVICE_ID, {0}},
};

int is_channel_busy(struct channel_data* channel)
{
    return channel->status != BUS_IDLE;
}

int is_die_busy(unsigned int channel_nr, unsigned int chip_nr,
                    unsigned int die_nr, int is_program)
{
    struct chip_data* chip;

    Xil_AssertNonvoid(channel_nr < NR_CHANNELS);
    Xil_AssertNonvoid(chip_nr < CHIPS_PER_CHANNEL);
    Xil_AssertNonvoid(die_nr < DIES_PER_CHIP);

    chip = &channel_data[channel_nr].chips[chip_nr];

    if (is_program) {
        /* A PROGRAM-series operation must be issued before the READ-series
         * operation for multi-LUN operations. */
        int i;

        for (i = 0; i < DIES_PER_CHIP; i++) {
            struct die_data* die = &chip->dies[i];

            if (die->active_cmd &&
                (die->active_cmd->cmd_code == FCMD_READ_PAGE ||
                 die->active_cmd->cmd_code == FCMD_READ_PAGE_MULTIPLANE))
                return TRUE;
        }
    }

    return chip->dies[die_nr].active_cmd != NULL;
}

static inline void set_channel_status(struct channel_data* channel,
                                      enum channel_status status)
{
    channel->status = status;
}

static inline void set_chip_status(struct chip_data* chip,
                                   enum chip_status status)
{
    chip->status = status;
}

static inline struct die_data* get_die(unsigned int channel, unsigned int chip,
                                       unsigned int die)
{
    Xil_AssertNonvoid(channel < NR_CHANNELS);
    Xil_AssertNonvoid(chip < CHIPS_PER_CHANNEL);
    Xil_AssertNonvoid(die < DIES_PER_CHIP);
    return &channel_data[channel].chips[chip].dies[die];
}

static inline void write_ce(struct chip_data* chip, int enable)
{
    XGpioPs_WritePin(&ps_gpio_inst, chip->ce_pin, !enable);
}

static inline void select_volume(struct chip_data* chip, int enable)
{
    int i;
    struct channel_data* channel = chip->channel;

    for (i = 0; i < CHIPS_PER_CHANNEL; i++) {
        write_ce(&channel->chips[i], enable);
    }

    if (enable) {
        nfc_cmd_volume_select(&channel->nfc, chip->index);
    }
}

static void init_die(struct die_data* die)
{
    die->active_cmd = NULL;
}

static void init_chip(struct chip_data* chip, int ce_pin)
{
    int i;

    chip->ce_pin = ce_pin;
    chip->nr_waiting_read_xfers = 0;

    for (i = 0; i < DIES_PER_CHIP; i++) {
        struct die_data* die = &chip->dies[i];
        die->index = i;
        die->chip = chip;

        init_die(die);
    }
}

static void init_channel(struct channel_data* channel)
{
    channel->status = BUS_IDLE;
}

static void alloc_flash_data(void)
{
    int ch_idx, chip_idx;
    int* ce_pp = ce_pins;
    struct channel_data* channel;
    struct chip_data* chip;

    for (ch_idx = 0; ch_idx < NR_CHANNELS; ch_idx++) {
        channel = &channel_data[ch_idx];

        /* Initialize channels first */
        channel->index = ch_idx;
        init_channel(channel);

        /* Match CEs to channels */
        for (chip_idx = 0; chip_idx < CHIPS_PER_CHANNEL; chip_idx++) {
            int ce_pin = *ce_pp++;

            chip = &channel->chips[chip_idx];
            chip->index = chip_idx;
            chip->channel = channel;
            init_chip(chip, ce_pin);
        }
    }
}

void panic(const char* fmt, ...)
{
    char buf[256];
    va_list arg;

    va_start(arg, fmt);
    vsprintf(buf, fmt, arg);
    va_end(arg);

    xil_printf("\nR5 panic: %s\n", buf);

    exit(1);
}

static void init_gpio(void)
{
    XGpioPs_Config* gpio_config_ptr;
    int i, status;

    gpio_config_ptr = XGpioPs_LookupConfig(XPAR_PSU_GPIO_0_DEVICE_ID);
    if (gpio_config_ptr == NULL) {
        panic("PS GPIO not found\n\r");
    }

    status = XGpioPs_CfgInitialize(&ps_gpio_inst, gpio_config_ptr,
                                   gpio_config_ptr->BaseAddr);
    if (status != XST_SUCCESS) {
        panic("PS GPIO init failed\n\r");
    }

    for (i = 0; i < sizeof(ce_pins) / sizeof(ce_pins[0]); i++) {
        XGpioPs_SetDirectionPin(&ps_gpio_inst, ce_pins[i], 1);
        XGpioPs_SetOutputEnablePin(&ps_gpio_inst, ce_pins[i], 1);
        XGpioPs_WritePin(&ps_gpio_inst, ce_pins[i], 1);
    }

    for (i = 0; i < sizeof(wp_pins) / sizeof(wp_pins[0]); i++) {
        XGpioPs_SetDirectionPin(&ps_gpio_inst, wp_pins[i], 1);
        XGpioPs_SetOutputEnablePin(&ps_gpio_inst, wp_pins[i], 1);
        XGpioPs_WritePin(&ps_gpio_inst, wp_pins[i], 1);
    }
}

int erase_block_simple(struct nf_controller* nfc, struct chip_data* chip,
                              unsigned int die, unsigned int plane,
                              unsigned int block)
{
    int ready, error;

    select_volume(chip, TRUE);
    nfc_cmd_erase_block(nfc, die, plane, block);

    do {
        ready = nfc_is_ready(nfc, die, plane, &error);
    } while (!(ready || error));

    select_volume(chip, FALSE);

    return error ? EIO : 0;
}

int program_page_simple(struct nf_controller* nfc,
                               struct chip_data* chip, unsigned int die,
                               unsigned int plane, unsigned int block,
                               unsigned int page, unsigned int col,
                               const u8* buffer, size_t count)
{
    int ready, error;

    select_volume(chip, TRUE);
    nfc_cmd_program_transfer(nfc, die, plane, block, page, col, buffer, count);
    while (!nfc_transfer_done(nfc, NFC_TO_NAND))
        ;
    nfc_complete_transfer(nfc, NFC_TO_NAND, count, NULL);
    nfc_cmd_program_page(nfc);

    do {
        ready = nfc_is_ready(nfc, die, plane, &error);
    } while (!(ready || error));

    select_volume(chip, FALSE);

    return error ? EIO : 0;
}

int read_page_simple(struct nf_controller* nfc, struct chip_data* chip,
                            unsigned int die, unsigned int plane,
                            unsigned int block, unsigned int page,
                            unsigned int col, u8* buffer, size_t count,
                            u8* code_buffer, u64* err_bitmap)
{
    int ready, error;

    select_volume(chip, TRUE);
    nfc_cmd_read_page_addr(nfc, die, plane, block, page, col);
    nfc_cmd_read_page(nfc);

    do {
        ready = nfc_is_ready(nfc, die, plane, &error);
    } while (!(ready || error));

    if (error) goto out;
    nfc_cmd_read_transfer(nfc, die, plane, buffer, count, code_buffer);
    while (!nfc_transfer_done(nfc, NFC_FROM_NAND))
        ;
    nfc_complete_transfer(nfc, NFC_FROM_NAND, count, err_bitmap);
    Xil_DCacheInvalidateRange((UINTPTR)buffer, count);

out:
    select_volume(chip, FALSE);

    return error ? EIO : 0;
}

static size_t read_page_test(struct nf_controller* nfc, struct chip_data* chip,
                             unsigned int die, unsigned int plane,
                             unsigned int block, unsigned int page,
                             unsigned int col, u8* buffer, size_t count,
                             u8* code_buffer, u64* err_bitmap,
                             const u8* gt_data, int bit)
{
    size_t err_count = 0;
    int i;

    read_page_simple(nfc, chip, die, plane, block, page, col, buffer, count,
                     code_buffer, err_bitmap);

    for (i = 0; i < count; i++) {
        if (bit == -1) {
            if (buffer[i] != gt_data[i]) err_count++;
        } else {
            if ((buffer[i] & (1 << bit)) != (gt_data[i] & (1 << bit)))
                err_count++;
        }
    }

    return err_count;
}

static int channel_selftest(struct channel_data* channel)
{
    static u8 buffer[2 * FLASH_PG_SIZE + FLASH_PG_OOB_SIZE]
        __attribute__((aligned(0x1000)));
    u8* tx_buffer;
    u8* rx_buffer;
    u8* code_buffer;
    struct nf_controller* nfc = &channel->nfc;
    int i;
    u64 err_bitmap[CHIPS_PER_CHANNEL];
    size_t err_count[CHIPS_PER_CHANNEL];

    tx_buffer = buffer;
    rx_buffer = &buffer[FLASH_PG_SIZE];
    code_buffer = &buffer[2 * FLASH_PG_SIZE];

    for (i = 0; i < FLASH_PG_SIZE; i++)
        tx_buffer[i] = i & 0xff;
    memset(rx_buffer, 0, FLASH_PG_SIZE);
    Xil_DCacheFlushRange((UINTPTR)tx_buffer, FLASH_PG_SIZE);
    Xil_DCacheFlushRange((UINTPTR)rx_buffer, FLASH_PG_SIZE);

    for (i = 0; i < CHIPS_PER_CHANNEL; i++) {
        struct chip_data* chip = &channel->chips[i];

        // Erase block.
        erase_block_simple(nfc, chip, 0, 0, TRAINING_BLOCK);

        // Program page.
        program_page_simple(nfc, chip, 0, 0, TRAINING_BLOCK, TRAINING_PAGE, 0,
                            tx_buffer, FLASH_PG_SIZE);
    }

    int k;
    for (k = 0; k < 10; k++) {
        for (i = 0; i < CHIPS_PER_CHANNEL; i++) {
            struct chip_data* chip = &channel->chips[i];

            err_count[i] = read_page_test(
                nfc, chip, 0, 0, TRAINING_BLOCK, TRAINING_PAGE, 0, rx_buffer,
                FLASH_PG_SIZE, code_buffer, &err_bitmap[i], tx_buffer, -1);
        }

        xil_printf("%d err (%d %d %d %d)\n", k, err_count[0], err_count[1],
                   err_count[2], err_count[3]);
    }

    /* for (i = 0; i < CHIPS_PER_CHANNEL; i++) { */
    /*     struct chip_data* chip = &channel->chips[i]; */
    /*     erase_block_simple(nfc, chip, 0, 0, TRAINING_BLOCK); */
    /* } */

    return TRUE;
}

static void reset_flash(void)
{
    int i, j;

    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;

        for (j = 0; j < CHIPS_PER_CHANNEL; j++) {
            struct chip_data* chip = &channel_data[i].chips[j];
            write_ce(chip, TRUE);
            nfc_cmd_reset(nfc);
            write_ce(chip, FALSE);
        }
    }

    usleep(5000);

    /* Appoint volume addresses. */
    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;

        for (j = 0; j < CHIPS_PER_CHANNEL; j++) {
            struct chip_data* chip = &channel_data[i].chips[j];
            write_ce(chip, TRUE);
            nfc_cmd_set_feature(nfc, 0x58, chip->index);
            usleep(1);
            write_ce(chip, FALSE);
        }
    }

    usleep(1);

    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;
        struct nfc_config* config = &nfc_configs[i];
        struct chip_data* chip;

        for (j = 0; j < CHIPS_PER_CHANNEL; j++) {
            if (config->odt_config[j] == 0) continue;

            chip = &channel_data[i].chips[j];
            write_ce(chip, TRUE);
            nfc_cmd_volume_select(nfc, chip->index);
            usleep(1);
            nfc_cmd_odt_configure(nfc, 0, config->odt_config[j]);
            usleep(1);
            write_ce(chip, FALSE);
        }
    }

    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;

        for (j = 0; j < CHIPS_PER_CHANNEL; j++) {
            struct chip_data* chip = &channel_data[i].chips[j];
            int nvddr2_feat = 0x47;

            write_ce(chip, TRUE);
            nfc_cmd_volume_select(nfc, chip->index);
            usleep(1);

            /* Configure RE/DQS differential signaling. */
            nfc_cmd_set_feature(nfc, 2, nvddr2_feat);
            usleep(1);
            /* Configure NV-DDR2 interface. */
            nfc_cmd_set_feature(nfc, 1, 0x27);
            usleep(1);

            write_ce(chip, FALSE);
        }

        nfc_cmd_enable_nvddr2(nfc);
    }

    usleep(10);

#ifdef NFC_SELFTEST
    for (i = 0; i < NR_CHANNELS; i++) {
        xil_printf("Channel %d: ", i);
        channel_selftest(&channel_data[i]);
    }
#endif
}

void fil_init(void)
{
    static u8 bdring_buffer[1 << 20] __attribute__((aligned(1 << 20)));
    int i;

    alloc_flash_data();

    init_gpio();

    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;
        struct nfc_config* config = &nfc_configs[i];

        nfc_init(nfc, (void*)config->base_addr, config->dma_dev_id, BCH_BLOCK_SIZE, BCH_CODE_SIZE);
    }

    reset_flash();

    INIT_LIST_HEAD(&finish_queue);
    INIT_LIST_HEAD(&wait_queue);
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

static int ecc_correct(u8* data, size_t data_length, u8* code, uint32_t* code_length, uint64_t err_bitmap){
	unsigned int i;
	int nblocks = ((data_length + BCH_BLOCK_SIZE - 1) / BCH_BLOCK_SIZE);
	size_t code_size = nblocks * BCH_CODE_SIZE;
	unsigned char calc_ecc[FLASH_PG_OOB_SIZE];
	int r;

	if (unlikely(!err_bitmap)) return 0;

	if (*code_length < code_size) {
		return -ENOMEM;
	}

	for (i = 0; i < nblocks; i++) {
		if (err_bitmap & (1ULL << i)) {
			bch_engine_calculate(&bch_engine, data + i * BCH_BLOCK_SIZE, calc_ecc + i * BCH_CODE_SIZE);
		}
	}

	for (i = 0; i < nblocks; i++) {
		if (err_bitmap & (1ULL << i)) {
			r = bch_engine_correct(&bch_engine, data + i * BCH_BLOCK_SIZE, code + i * BCH_CODE_SIZE, calc_ecc + i * BCH_CODE_SIZE);
			if (r < 0) break;
		}
	}

	*code_length = code_size;

	return r;
}

void scan_badblocks(){
	static u8 buffer[2 * FLASH_PG_SIZE + FLASH_PG_OOB_SIZE]__attribute__((aligned(0x1000)));
	u8* tx_buffer = buffer;
	u8* rx_buffer = &buffer[FLASH_PG_SIZE];
	u8* code_buffer = &buffer[2 * FLASH_PG_SIZE];
	int i, j, die, plane, block;
	int bad_count = 0;
	int good_count = 0;
	for (i = 0; i < NR_CHANNELS; i++){
		struct channel_data* channel = &channel_data[i];
		struct nf_controller* nfc = &channel->nfc;
		xil_printf("[Debug] Channel Num: %d\n", i);
		for (j = 0; j < CHIPS_PER_CHANNEL; j++){
			struct chip_data* chip = &channel->chips[j];
			for (die= 0; die < DIES_PER_CHIP; die++){
				for (plane = 0; plane < PLANES_PER_DIE; plane++) {
					for (block = 0; block < BLOCKS_PER_PLANE; block++){
						int bad = 0;

						int err = read_page_simple(nfc, chip, die, plane, block, 0, FLASH_PG_SIZE, rx_buffer, nfc->step_size, NULL, NULL);
						bad = (err != 0 || rx_buffer[0] == 0);
						if (bad){
							bad_count++;
							xil_printf("[Debug] Bad Block: %d channel, %d chip, %d die, %d plane, %d block\n", i, j, die, plane, block);
						} else {
							good_count++;
						}
					}
				}
			}
		}
	}
	xil_printf("[Debug] Good Block Count: %d\n", good_count);
}

int die_cmd_in(struct die_data* die) {
	flash_transaction* txn;
	struct nf_controller* nfc;
	struct channel_data* channel;

	txn = die->active_txn;
	channel = die->chip->channel;
	nfc = channel->nfc;

	if (is_channel_busy(die->chip->channel)) {
		return 0;
	}
	switch (txn->fil_task_level) {
	case READ_MISS:
		nfc_cmd_read_page_addr(nfc, txn->fil_inuse_addr.die, txn->fil_inuse_addr.plane, txn->fil_inuse_addr.block, txn->fil_inuse_addr.page, 0);
		nfc_cmd_read_page(nfc);
		die->status = DS_CMD_EXE;
		break;
	case WRITE_BACK:
		unsigned char* buffer = txn->victim_data;
		nfc_cmd_program_transfer(nfc, txn->fil_inuse_addr.die, txn->fil_inuse_addr.plane, txn->fil_inuse_addr.block, txn->fil_inuse_addr.page, 0, buffer, FLASH_PG_SIZE);
		set_channel_status(channel, BUS_BUSY);
		die->status = DS_DATA_IN;
		break;
	case TXN_ERASE:
		nfc_cmd_erase_block(nfc, txn->addr.die, txn->addr.plane, txn->addr.block);
		die->status = DS_CMD_EXE;
		break;
	}
	return 1;
}

int primary_dispatch_request(flash_transaction* txn) {
	txn->fil_task_level = WRITE_BACK;
	ppa_to_address(txn->evict_ppa, &txn->fil_inuse_addr);
	list_add_tail(&(txns->txn_list_handle), &(channel_data[addr.channel].chips[addr.chip].txn_queue));
}

int secondary_dispatch_request(flash_transaction* txn) {
	txn->fil_task_level = READ_MISS;
	ppa_to_address(txn->ppa, &txn->fil_inuse_addr);
	list_add_tail(&(txns->txn_list_handle), &(channel_data[addr.channel].chips[addr.chip].txn_queue));
}

int finish_dispatch_request(flash_transaction* txn) {
	txn->fil_task_level = FINISH;
	list_add_tail((txns->txn_list_handle), &finish_queue);
}

int is_obey_cache_line_dependency(flash_transaction* txn) {
	int cache_line_index = txn->lpa_page % CACHE_SIZE;

	if (cache_lines[cache_line_index].req_id == txn->req_id_victim) {
		return 1;
	}
	return 0;
}

void merge_data_buffer(flash_transaction* txn, unsigned char* buffer) {
	void *dst, *src;
	size_t len;

	dst = (void*)txn->data;
	src = (void*)buffer;
	len = (size_t)txn->offset;
	memcpy(dst, src, len);

	len += txn->length;
	if (len < FLASH_PG_SIZE) {
		dst += len;
		src += len;
		len = FLASH_PG_SIZE - len;
		memcpy(dst, src, len);
	}
}

int re_dispatch_request_otherwise_finish(flash_transaction* txn) {
	int finish = 0;

	switch (txn->fil_task_level) {
	case WRITE_BACK:
		if (txn->cache_hit == HIT || (txn->txn_type == TXN_WRITE && txn->offset == 0 && txn->length == FLASH_PG_SIZE)) {
			finish = 1;
		}
		else {
			secondary_dispatch_request(txn);
		}
		break;
	case READ_MISS:
		finish = 1;
		break;
	}

	return finish;
}

int chip_schedule_req(struct chip_data* chip) {
	struct die_data* target_die;
	flash_transaction* txn;

	target_die = chip->dies[chip->current_active_die];
	txn = target_die->active_txn;
	if (txn && re_dispatch_request_otherwise_finish(txn)){
		finish_dispatch_request(txn);
	}

	if (!list_empty(chip->txn_queue)) {
		txn = list_first_entry(chip->txn_queue, flash_transaction, txn_list_handle);
		chip->current_active_die = txn->fil_inuse_addr.die;
		chip->dies[chip->current_active_die].active_txn = txn;
		list_del(txn->txn_list_handle);
		die_cmd_in(target_die);
	}
}

int die_data_FLASH(struct die_data* die) {
	flash_transaction* txn;
	struct nf_controller* nfc;
	struct channel_data* channel;

	txn = die->active_txn;
	channel = die->chip->channel;
	nfc = channel->nfc;

	if (is_channel_busy(die->chip->channel)) {
		return 0;
	}
	switch (txn->fil_task_level) {
	case READ_MISS:
		int ready = nfc_is_ready(nfc, die, txn->fil_inuse_addr.plane, NULL);
		if (ready) {
			unsigned char *buffer, *code_buffer;
			if (txn->type == TXN_READ) {
				buffer = txn->data;
				code_buffer = txn->data + FLASH_PG_SIZE;
			} else {
				buffer = die->buffer;
				code_buffer = die->code_buffer;
			}
			nfc_cmd_read_transfer(nfc, die, txn->fil_inuse_addr.plane, buffer, FLASH_PG_SIZE, code_buffer);
			set_channel_status(channel, BUS_BUSY);
			die->status = DS_DATA_OUT;
		}
		break;
	case WRITE_BACK:
		int ready = nfc_is_ready(nfc, die, txn->fil_inuse_addr.plane, NULL);
		if (ready) {
			die->status = DS_IDLE;
			chip_schedule_req(die->chip);
		}
	}
	return 1;
}

int die_data_DMA(struct die_data* die) {
	flash_transaction* txn;
	struct nf_controller* nfc;
	struct channel_data* channel;

	txn = die->active_txn;
	channel = die->chip->channel;
	nfc = channel->nfc;

	switch (txn->fil_task_level) {
	case READ_MISS:
		int ready = nfc_transfer_done(nfc, NFC_FROM_NAND);
		if (ready) {
			unsigned char *buffer, *code_buffer;
			if (txn->type == TXN_READ) {
				buffer = txn->data;
				code_buffer = txn->data + FLASH_PG_SIZE;
			} else {
				buffer = die->buffer;
				code_buffer = die->code_buffer;
			}
			set_channel_status(channel, BUS_IDLE);
		    nfc_complete_transfer(nfc, NFC_FROM_NAND, FLASH_PG_SIZE, txn->err_bitmap);
		    Xil_DCacheInvalidateRange(buffer, FLASH_PG_SIZE);
		    if (txn->err_bitmap) {
		    	// TODO ecc correct
		    }
		    // merge
		    if (txn->type == TXN_WRITE) {
		    	merge_data_buffer(txn, buffer);
		    }
		    die->status = DS_IDLE;
		    chip_schedule_req(die->chip);
		}
		break;
	case WRITE_BACK:
		int ready = nfc_transfer_done(nfc, NFC_TO_NAND);
		if (ready) {
			set_channel_status(channel, BUS_IDLE);
			nfc_cmd_program_page(nfc);
			die->status = DS_CMD_EXE;
		}
		break;
	}
	return 1;
}

int die_tick(struct die_data* die){
	switch(die->status){
	case DS_IDLE:
		chip_schedule_req(die);
	case DS_CMD_EXE:
		die_data_FLASH(die);
		break;
	case DS_DATA_IN:
		die_data_DMA(die);
		break;
	case DS_DATA_OUT:
		die_data_DMA(die);
		break;
	default:
		break;
	}
	return 1;
}

/*
 * fetch requests from pipeline queue and append them to chip queue.
 */
void dispatch_requests_taskQ(flash_transaction *txns, int number){
	struct flash_address addr;

	for (int i = 0; i < number; i++) {
		ppa_to_address(txns->ppa, &addr);
		INIT_LIST_HEAD(txns->txn_list_handle);
		if (txns->req_id_victim == NO_VICTIM) {
			// go to secondary dispatch
			if (txns->hit == MISS_DIRTY || txns->hit == MISS_NONDIRTY) {
				secondary_dispatch_request(txns);
			}
		} else {
			// go to check cache line dependency
			if (is_obey_cache_line_dependency(txns)) {
				primary_dispatch_request(txns);
			} else {
				// go to the wait list
				list_add_tail(&(txns->txn_list_handle), &wait_queue);
			}
		}
		txns++;
	}
}

void dispatch_requests_waitQ() {
	flash_transaction* txn;

	list_for_each_entry(txn, &wait_queue, txn_list_handle) {
		if (obey_cache_line_dependency(txn)) {
			if (txns->cache_line_bitmap != FULL_CACHELINE) {
				preread_dispatch_request(txns);
			} else {
				primary_dispatch_request(txns);
			}
		}
	}
}

void dispatch_cmds() {
	struct channel_data *channel;
	struct nf_controller *nfc;
	struct chip_data *chip;
	struct die_data *die;
	for (int i = 0; i < NR_CHANNELS; i++) {
		channel = &channel_data[i];
		nfc = &channel->nfc;
		for (int j = 0; j < CHIPS_PER_CHANNEL; j++) {
			chip = &channel->chips[j];
			die = chip->dies[pchip->current_active_die];
			select_volume(chip, TRUE);
			die_tick(die);
			select_volume(chip, FALSE);
		}
	}
}
