#ifndef GZY_ACCELERATOR_TOP_AXI_H
#define GZY_ACCELERATOR_TOP_AXI_H

#include "accelerator_instruction.h"

extern "C" {
int accelerator_top_axi(
    accel_instr_word_t instr_mem[ACCEL_MAX_INSTR],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int instr_num
);
}

#endif
