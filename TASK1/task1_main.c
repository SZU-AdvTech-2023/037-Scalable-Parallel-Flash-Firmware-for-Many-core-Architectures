#include "bitmap.h"
#include "request_types.h"
#include "req_queue.h"
#include "config.h"
#include "block_manager.h"
#include "ff.h"


struct ReQ* in_queue = (struct ReQ*)REQ_1_QUEUE_META_BASE;
struct ReQ* out_queue = (struct ReQ*)REQ_2_QUEUE_META_BASE;

static int format_emmc(void)
{
    static BYTE work[FF_MAX_SS];
    return f_mkfs("", FM_FAT32, 0, work, sizeof work);
}

static inline int init_emmc(void) { return f_mount(&fatfs, "", 0); }

int main(){
	//TODO: init

	ReQ_init(in_queue, (u8*)REQ_1_QUEUE_DATA_BASE, 1024, sizeof(flash_transaction));
	int status = 0;
	format_emmc();
	status = init_emmc();
    if (status != 0) {
        xil_printf("Failed to initialize EMMC\n");
        return XST_FAILURE;
    }
	domain_init(1);
	while(1){
		flash_transaction txn;
		ReQ_fetch(in_queue, (void*)&txn, sizeof(txn));
		translate_ppa(&txn);
		ReQ_append(out_queue, (void*)&txn, sizeof(txn));
	}
}
