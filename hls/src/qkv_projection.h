#ifndef GZY_QKV_PROJECTION_H
#define GZY_QKV_PROJECTION_H

#include "gemm_types.h"

void qkv_projection(
    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t Q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t K_out[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t V[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
);

extern "C" {
void qkv_top(
    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t Q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t K_out[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t V[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
);
}

#endif
