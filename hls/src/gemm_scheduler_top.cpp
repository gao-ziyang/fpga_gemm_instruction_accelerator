#include "gemm_scheduler.h"

extern "C" {
void gemm_scheduler_top(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS]
) {
#pragma HLS INTERFACE ap_memory port=A_mem
#pragma HLS INTERFACE ap_memory port=B_mem
#pragma HLS INTERFACE ap_memory port=C_mem
#pragma HLS INTERFACE ap_ctrl_hs port=return

    gemm_scheduler(
        A_mem,
        B_mem,
        C_mem,
        ACCEL_BENCH_N,
        ACCEL_BENCH_K,
        ACCEL_BENCH_M,
        0,
        0,
        0
    );
}
}
