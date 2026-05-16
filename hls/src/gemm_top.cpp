#include "gemm_core.h"

extern "C" {
void gemm_top(
    gemm_data_t A[GEMM_DIM][GEMM_DIM],
    gemm_data_t B[GEMM_DIM][GEMM_DIM],
    gemm_acc_t C[GEMM_DIM][GEMM_DIM]
) {
#pragma HLS INTERFACE ap_memory port=A
#pragma HLS INTERFACE ap_memory port=B
#pragma HLS INTERFACE ap_memory port=C
#pragma HLS INTERFACE ap_ctrl_hs port=return

    gemm_4x4(A, B, C);
}
}
