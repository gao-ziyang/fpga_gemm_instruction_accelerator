#include "accelerator_top_axi.h"

extern "C" {
int accelerator_top_axi(
    accel_instr_word_t instr_mem[ACCEL_MAX_INSTR],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int instr_num
) {
#pragma HLS INTERFACE m_axi port=instr_mem offset=slave bundle=gmem depth=ACCEL_MAX_INSTR
#pragma HLS INTERFACE m_axi port=A_mem offset=slave bundle=gmem depth=ACCEL_A_ELEMS
#pragma HLS INTERFACE m_axi port=B_mem offset=slave bundle=gmem depth=ACCEL_B_ELEMS
#pragma HLS INTERFACE m_axi port=C_mem offset=slave bundle=gmem depth=ACCEL_C_ELEMS

#pragma HLS INTERFACE s_axilite port=instr_mem bundle=control
#pragma HLS INTERFACE s_axilite port=A_mem bundle=control
#pragma HLS INTERFACE s_axilite port=B_mem bundle=control
#pragma HLS INTERFACE s_axilite port=C_mem bundle=control
#pragma HLS INTERFACE s_axilite port=instr_num bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    return execute_instruction_stream(
        instr_mem,
        A_mem,
        B_mem,
        C_mem,
        instr_num
    );
}
}
