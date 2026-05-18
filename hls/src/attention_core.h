#ifndef GZY_ATTENTION_CORE_H
#define GZY_ATTENTION_CORE_H

#include "gemm_core.h"
#include "qkv_projection.h"

static const int ATTENTION_P_SHIFT = 6;

gemm_data_t saturate_to_int8(gemm_acc_t x, int shift);

void quantize_qkv(
    gemm_acc_t Q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t K_out[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t V[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t V_q[GEMM_MAX_K][GEMM_MAX_M],
    int N,
    int D,
    int q_shift
);

void transpose_K(
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t K_T[GEMM_MAX_K][GEMM_MAX_M],
    int N,
    int D
);

void attention_score_core(
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
);

void row_normalize_score(
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t P_q[GEMM_MAX_N][GEMM_MAX_K],
    int N,
    int p_shift
);

void attention_no_softmax_core(
    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t Out[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D,
    int q_shift,
    int score_shift
);

void attention_core(
    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t Out[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D,
    int q_shift,
    int p_shift
);

extern "C" {
void attention_score_top(
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
);

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
);

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
);
}

#endif
