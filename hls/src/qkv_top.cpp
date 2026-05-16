#include "qkv_projection.h"

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
) {
#pragma HLS INTERFACE ap_memory port=X
#pragma HLS INTERFACE ap_memory port=Wq
#pragma HLS INTERFACE ap_memory port=Wk
#pragma HLS INTERFACE ap_memory port=Wv
#pragma HLS INTERFACE ap_memory port=Q
#pragma HLS INTERFACE ap_memory port=K_out
#pragma HLS INTERFACE ap_memory port=V
#pragma HLS INTERFACE ap_ctrl_hs port=return

    qkv_projection(X, Wq, Wk, Wv, Q, K_out, V, N, D);
}
}
