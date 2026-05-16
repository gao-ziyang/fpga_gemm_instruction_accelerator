#include "gemm_core.h"

extern "C" {
void gemm_top(
    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int K,
    int M,
    bool update_A
) {
#pragma HLS INTERFACE ap_memory port=A
#pragma HLS INTERFACE ap_memory port=B
#pragma HLS INTERFACE ap_memory port=C
#pragma HLS INTERFACE ap_ctrl_hs port=return

    gemm_tiled(A, B, C, N, K, M, update_A);
}
}
