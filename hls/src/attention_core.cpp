#include "attention_core.h"

gemm_data_t saturate_to_int8(gemm_acc_t x, int shift) {
#pragma HLS INLINE
    gemm_acc_t y = x;
    if (shift > 0) {
        y = x >> shift;
    }

    if (y > 127) {
        return (gemm_data_t)127;
    }
    if (y < -128) {
        return (gemm_data_t)-128;
    }
    return (gemm_data_t)y;
}

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
) {
#pragma HLS INLINE off
quantize_loop_i:
    for (int i = 0; i < N; i++) {
        for (int d = 0; d < D; d++) {
#pragma HLS PIPELINE II=1
            Q_q[i][d] = saturate_to_int8(Q[i][d], q_shift);
            K_q[i][d] = saturate_to_int8(K_out[i][d], q_shift);
            V_q[i][d] = saturate_to_int8(V[i][d], q_shift);
        }
    }
}

void transpose_K(
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t K_T[GEMM_MAX_K][GEMM_MAX_M],
    int N,
    int D
) {
#pragma HLS INLINE off
transpose_d:
    for (int d = 0; d < D; d++) {
        for (int n = 0; n < N; n++) {
#pragma HLS PIPELINE II=1
            K_T[d][n] = K_q[n][d];
        }
    }
}

void attention_score_core(
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
) {
#pragma HLS INLINE off
    static gemm_data_t K_T[GEMM_MAX_K][GEMM_MAX_M];

    transpose_K(K_q, K_T, N, D);
    gemm_tiled(Q_q, K_T, Score, N, D, N, true);
}

static void quantize_score_to_int8(
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t Score_q[GEMM_MAX_N][GEMM_MAX_K],
    int N,
    int score_shift
) {
#pragma HLS INLINE off
score_q_i:
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
#pragma HLS PIPELINE II=1
            Score_q[i][j] = saturate_to_int8(Score[i][j], score_shift);
        }
    }
}

void row_normalize_score(
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t P_q[GEMM_MAX_N][GEMM_MAX_K],
    int N,
    int p_shift
) {
#pragma HLS INLINE off
row_norm_i:
    for (int i = 0; i < N; i++) {
        gemm_acc_t row_sum = 0;

    row_sum_j:
        for (int j = 0; j < N; j++) {
#pragma HLS PIPELINE II=1
            gemm_acc_t positive = Score[i][j] > 0 ? Score[i][j] : (gemm_acc_t)0;
            row_sum += positive;
        }

    row_norm_j:
        for (int j = 0; j < N; j++) {
#pragma HLS PIPELINE II=1
            gemm_acc_t positive = Score[i][j] > 0 ? Score[i][j] : (gemm_acc_t)0;
            if (row_sum == 0) {
                P_q[i][j] = 0;
            } else {
                gemm_acc_t scaled = (positive << p_shift) / row_sum;
                if (scaled > 127) {
                    scaled = 127;
                }
                P_q[i][j] = (gemm_data_t)scaled;
            }
        }
    }
}

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
) {
#pragma HLS INLINE off
    static gemm_acc_t Q[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_acc_t K_out[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_acc_t V[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K];
    static gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_data_t V_q[GEMM_MAX_K][GEMM_MAX_M];
    static gemm_data_t Score_q[GEMM_MAX_N][GEMM_MAX_K];

    qkv_projection(X, Wq, Wk, Wv, Q, K_out, V, N, D);
    quantize_qkv(Q, K_out, V, Q_q, K_q, V_q, N, D, q_shift);
    attention_score_core(Q_q, K_q, Score, N, D);
    quantize_score_to_int8(Score, Score_q, N, score_shift);
    gemm_tiled(Score_q, V_q, Out, N, N, D, true);
}

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
) {
#pragma HLS INLINE off
    static gemm_acc_t Q[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_acc_t K_out[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_acc_t V[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K];
    static gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M];
    static gemm_data_t V_q[GEMM_MAX_K][GEMM_MAX_M];
    static gemm_data_t P_q[GEMM_MAX_N][GEMM_MAX_K];

    qkv_projection(X, Wq, Wk, Wv, Q, K_out, V, N, D);
    quantize_qkv(Q, K_out, V, Q_q, K_q, V_q, N, D, q_shift);
    attention_score_core(Q_q, K_q, Score, N, D);
    row_normalize_score(Score, P_q, N, p_shift);
    gemm_tiled(P_q, V_q, Out, N, N, D, true);
}
