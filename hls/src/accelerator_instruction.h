#ifndef GZY_ACCELERATOR_INSTRUCTION_H
#define GZY_ACCELERATOR_INSTRUCTION_H

#include "accelerator_types.h"

struct decoded_instr_t {
    ap_uint<8> opcode;
    int N;
    int K;
    int M;
    int a_base;
    int b_base;
    int c_base;
};

decoded_instr_t decode_instruction(accel_instr_word_t word);

int execute_instruction_stream(
    accel_instr_word_t instr_mem[ACCEL_MAX_INSTR],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int instr_num
);

extern "C" {
void instruction_decode_top(
    accel_instr_word_t instr_mem[ACCEL_MAX_INSTR],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int status[1]
);
}

#endif
