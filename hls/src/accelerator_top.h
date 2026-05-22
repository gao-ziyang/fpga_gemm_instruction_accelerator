#ifndef GZY_ACCELERATOR_TOP_H
#define GZY_ACCELERATOR_TOP_H

#include "accelerator_instruction.h"

extern "C" {
void accelerator_top(
    accel_instr_word_t instr_mem[ACCEL_MAX_INSTR],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int instr_num,
    int status[1]
);
}

#endif
