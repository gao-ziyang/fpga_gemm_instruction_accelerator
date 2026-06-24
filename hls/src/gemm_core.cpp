#include "gemm_core.h"

void gemm_core_mac(
    gemm_data_t localA[GEMM_TILE][GEMM_TILE],
    gemm_data_t localB[GEMM_TILE][GEMM_TILE],
    gemm_acc_t localC[GEMM_TILE][GEMM_TILE]
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0
#pragma HLS ARRAY_PARTITION variable=localC complete dim=0

core_k_loop:
    for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS PIPELINE II=1
    core_i_loop:
        for (int ii = 0; ii < GEMM_TILE; ii++) {
#pragma HLS UNROLL
            gemm_acc_t a_val = (gemm_acc_t)localA[ii][kk];
        core_j_loop:
            for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                gemm_acc_t b_val = (gemm_acc_t)localB[kk][jj];
                localC[ii][jj] += a_val * b_val;
            }
        }
    }
}
