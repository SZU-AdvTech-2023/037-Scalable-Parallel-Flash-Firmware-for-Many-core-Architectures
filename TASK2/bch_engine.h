#ifndef _BCH_ENGINE_H_
#define _BCH_ENGINE_H_

#include "bch.h"

#define ETS_OK        0
#define ETS_DEC_ERROR 1
#define ETS_NOSPC     2

struct bch_engine {
    struct bch_control* bch;
    unsigned char* ecc_mask;
    unsigned int* err_loc;
    unsigned int step_size;
    unsigned int code_size;
};

int bch_engine_init(struct bch_engine* engine, unsigned int ecc_size,
                    unsigned int ecc_bytes);
void bch_engine_cleanup(struct bch_engine* engine);
int bch_engine_calculate(struct bch_engine* engine, const unsigned char* buf,
                         unsigned char* code);
int bch_engine_correct(struct bch_engine* engine, unsigned char* buf,
                       const unsigned char* read_ecc,
                       const unsigned char* calc_ecc);

#endif
