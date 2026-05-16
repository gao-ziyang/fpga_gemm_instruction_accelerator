#ifndef GZY_GEMM_CORE_H
#define GZY_GEMM_CORE_H

#include "gemm_types.h"

void gemm_4x4(
    gemm_data_t A[GEMM_DIM][GEMM_DIM],
    gemm_data_t B[GEMM_DIM][GEMM_DIM],
    gemm_acc_t C[GEMM_DIM][GEMM_DIM]
);

extern "C" {
void gemm_top(
    gemm_data_t A[GEMM_DIM][GEMM_DIM],
    gemm_data_t B[GEMM_DIM][GEMM_DIM],
    gemm_acc_t C[GEMM_DIM][GEMM_DIM]
);
}

#endif
