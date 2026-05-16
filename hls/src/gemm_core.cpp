#include "gemm_core.h"

void gemm_4x4(
    gemm_data_t A[GEMM_DIM][GEMM_DIM],
    gemm_data_t B[GEMM_DIM][GEMM_DIM],
    gemm_acc_t C[GEMM_DIM][GEMM_DIM]
) {
#pragma HLS INLINE off

    gemm_data_t localA[GEMM_DIM][GEMM_DIM];
    gemm_data_t localB[GEMM_DIM][GEMM_DIM];

#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0

load_mats:
    for (int i = 0; i < GEMM_DIM; i++) {
        for (int j = 0; j < GEMM_DIM; j++) {
#pragma HLS PIPELINE II=1
            localA[i][j] = A[i][j];
            localB[i][j] = B[i][j];
        }
    }

row_loop:
    for (int i = 0; i < GEMM_DIM; i++) {
    col_loop:
        for (int j = 0; j < GEMM_DIM; j++) {
#pragma HLS PIPELINE II=1
            gemm_acc_t sum = 0;

        dot_loop:
            for (int k = 0; k < GEMM_DIM; k++) {
#pragma HLS UNROLL
                sum += (gemm_acc_t)localA[i][k] * (gemm_acc_t)localB[k][j];
            }

            C[i][j] = sum;
        }
    }
}
