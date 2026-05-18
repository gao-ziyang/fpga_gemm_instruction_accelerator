#include "attention_core.h"

extern "C" {
void attention_score_top(
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
) {
#pragma HLS INTERFACE ap_memory port=Q_q
#pragma HLS INTERFACE ap_memory port=K_q
#pragma HLS INTERFACE ap_memory port=Score
#pragma HLS INTERFACE ap_ctrl_hs port=return

    attention_score_core(Q_q, K_q, Score, N, D);
}

void attention_no_softmax_top(
    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t Out[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D,
    int q_shift,
    int score_shift
) {
#pragma HLS INTERFACE ap_memory port=X
#pragma HLS INTERFACE ap_memory port=Wq
#pragma HLS INTERFACE ap_memory port=Wk
#pragma HLS INTERFACE ap_memory port=Wv
#pragma HLS INTERFACE ap_memory port=Out
#pragma HLS INTERFACE ap_ctrl_hs port=return

    attention_no_softmax_core(X, Wq, Wk, Wv, Out, N, D, q_shift, score_shift);
}

void attention_top(
    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t Out[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D,
    int q_shift,
    int p_shift
) {
#pragma HLS INTERFACE ap_memory port=X
#pragma HLS INTERFACE ap_memory port=Wq
#pragma HLS INTERFACE ap_memory port=Wk
#pragma HLS INTERFACE ap_memory port=Wv
#pragma HLS INTERFACE ap_memory port=Out
#pragma HLS INTERFACE ap_ctrl_hs port=return

    attention_core(X, Wq, Wk, Wv, Out, N, D, q_shift, p_shift);
}
}
