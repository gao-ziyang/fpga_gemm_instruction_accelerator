#include "accelerator_top.h"

extern "C" {
void accelerator_top(
    accel_instr_word_t instr_mem[ACCEL_MAX_INSTR],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int instr_num,
    int status[1]
) {
#pragma HLS INTERFACE ap_memory port=instr_mem
#pragma HLS INTERFACE ap_memory port=A_mem
#pragma HLS INTERFACE ap_memory port=B_mem
#pragma HLS INTERFACE ap_memory port=C_mem
#pragma HLS INTERFACE ap_memory port=status
#pragma HLS INTERFACE ap_none port=instr_num
#pragma HLS INTERFACE ap_ctrl_hs port=return

    status[0] = execute_instruction_stream(
        instr_mem,
        A_mem,
        B_mem,
        C_mem,
        instr_num
    );
}
}
