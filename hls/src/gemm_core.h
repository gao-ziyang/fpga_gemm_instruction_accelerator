#ifndef GZY_GEMM_CORE_H
#define GZY_GEMM_CORE_H

#include "gemm_types.h"

void gemm_tiled(
    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int K,
    int M,
    bool update_A
);

extern "C" {
void gemm_top(
    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int K,
    int M,
    bool update_A
);
}

#endif
