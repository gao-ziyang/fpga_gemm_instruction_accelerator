#include "gemm_core.h"
#include "qkv_projection.h"

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
) {
#pragma HLS INLINE off

    gemm_tiled(X, Wq, Q, N, D, D, true);
    gemm_tiled(X, Wk, K_out, N, D, D, false);
    gemm_tiled(X, Wv, V, N, D, D, false);
}
