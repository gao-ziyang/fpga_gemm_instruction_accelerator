#include <cstdio>

#include "attention_core.h"

static gemm_data_t ref_saturate_to_int8(gemm_acc_t x, int shift) {
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

static gemm_data_t gen_x(int i, int k) {
    return (gemm_data_t)(((i * 19 + k * 11 + 9) % 96) - 48);
}

static gemm_data_t gen_w(int k, int j, int seed) {
    return (gemm_data_t)(((k * 17 + j * 23 + seed) % 96) - 48);
}

static void clear_data_nm(gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K]) {
    for (int i = 0; i < GEMM_MAX_N; i++) {
        for (int j = 0; j < GEMM_MAX_K; j++) {
            A[i][j] = 0;
        }
    }
}

static void clear_data_km(gemm_data_t A[GEMM_MAX_K][GEMM_MAX_M]) {
    for (int i = 0; i < GEMM_MAX_K; i++) {
        for (int j = 0; j < GEMM_MAX_M; j++) {
            A[i][j] = 0;
        }
    }
}

static void clear_acc(gemm_acc_t A[GEMM_MAX_N][GEMM_MAX_M]) {
    for (int i = 0; i < GEMM_MAX_N; i++) {
        for (int j = 0; j < GEMM_MAX_M; j++) {
            A[i][j] = 0;
        }
    }
}

static void init_input_weights(
    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M],
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M],
    int N,
    int D
) {
    clear_data_nm(X);
    clear_data_km(Wq);
    clear_data_km(Wk);
    clear_data_km(Wv);

    for (int i = 0; i < N; i++) {
        for (int k = 0; k < D; k++) {
            X[i][k] = gen_x(i, k);
        }
    }

    for (int k = 0; k < D; k++) {
        for (int j = 0; j < D; j++) {
            Wq[k][j] = gen_w(k, j, 5);
            Wk[k][j] = gen_w(k, j, 17);
            Wv[k][j] = gen_w(k, j, 31);
        }
    }
}

static void reference_projection(
    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t W[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t Y[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < D; j++) {
            gemm_acc_t acc = 0;
            for (int k = 0; k < D; k++) {
                acc += (gemm_acc_t)X[i][k] * (gemm_acc_t)W[k][j];
            }
            Y[i][j] = acc;
        }
    }
}

static void reference_quantize_qkv(
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
    clear_data_nm(Q_q);
    clear_data_km(V_q);
    for (int i = 0; i < GEMM_MAX_N; i++) {
        for (int j = 0; j < GEMM_MAX_M; j++) {
            K_q[i][j] = 0;
        }
    }

    for (int i = 0; i < N; i++) {
        for (int d = 0; d < D; d++) {
            Q_q[i][d] = ref_saturate_to_int8(Q[i][d], q_shift);
            K_q[i][d] = ref_saturate_to_int8(K_out[i][d], q_shift);
            V_q[i][d] = ref_saturate_to_int8(V[i][d], q_shift);
        }
    }
}

static void reference_score(
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            gemm_acc_t acc = 0;
            for (int d = 0; d < D; d++) {
                acc += (gemm_acc_t)Q_q[i][d] * (gemm_acc_t)K_q[j][d];
            }
            Score[i][j] = acc;
        }
    }
}

static void reference_quantize_score(
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t Score_q[GEMM_MAX_N][GEMM_MAX_K],
    int N,
    int score_shift
) {
    clear_data_nm(Score_q);
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            Score_q[i][j] = ref_saturate_to_int8(Score[i][j], score_shift);
        }
    }
}

static void reference_row_normalize(
    gemm_acc_t Score[GEMM_MAX_N][GEMM_MAX_M],
    gemm_data_t P_q[GEMM_MAX_N][GEMM_MAX_K],
    int N,
    int p_shift
) {
    clear_data_nm(P_q);
    for (int i = 0; i < N; i++) {
        gemm_acc_t row_sum = 0;
        for (int j = 0; j < N; j++) {
            gemm_acc_t positive = Score[i][j] > 0 ? Score[i][j] : (gemm_acc_t)0;
            row_sum += positive;
        }

        for (int j = 0; j < N; j++) {
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

static void reference_out(
    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t Out[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int K,
    int M
) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < M; j++) {
            gemm_acc_t acc = 0;
            for (int k = 0; k < K; k++) {
                acc += (gemm_acc_t)A[i][k] * (gemm_acc_t)B[k][j];
            }
            Out[i][j] = acc;
        }
    }
}

static int compare_acc(
    const char* name,
    gemm_acc_t got[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t expected[GEMM_MAX_N][GEMM_MAX_M],
    int rows,
    int cols,
    int* max_abs_error,
    long long* checksum,
    int offset
) {
    int errors = 0;
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            int got_i = (int)got[i][j];
            int ref_i = (int)expected[i][j];
            int diff = got_i - ref_i;
            int abs_diff = diff < 0 ? -diff : diff;
            if (abs_diff > *max_abs_error) {
                *max_abs_error = abs_diff;
            }
            *checksum += (long long)got_i * (long long)(offset + i * cols + j + 1);
            if (got_i != ref_i) {
                std::printf("[ERR] %s[%d][%d] got %d, expected %d\n", name, i, j, got_i, ref_i);
                errors++;
            }
        }
    }
    return errors;
}

static int validate_saturate() {
    struct Case {
        gemm_acc_t x;
        int shift;
        int expected;
    };

    const Case cases[] = {
        {(gemm_acc_t)0, 0, 0},
        {(gemm_acc_t)127, 0, 127},
        {(gemm_acc_t)128, 0, 127},
        {(gemm_acc_t)-129, 0, -128},
        {(gemm_acc_t)512, 2, 127},
        {(gemm_acc_t)-512, 2, -128},
        {(gemm_acc_t)64, 1, 32},
        {(gemm_acc_t)-64, 1, -32},
    };

    int errors = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = (int)saturate_to_int8(cases[i].x, cases[i].shift);
        if (got != cases[i].expected) {
            std::printf(
                "[ERR] saturate case %u got %d, expected %d\n",
                i, got, cases[i].expected
            );
            errors++;
        }
    }
    return errors;
}

static void init_direct_qk(
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D
) {
    clear_data_nm(Q_q);
    for (int i = 0; i < GEMM_MAX_N; i++) {
        for (int j = 0; j < GEMM_MAX_M; j++) {
            K_q[i][j] = 0;
        }
    }

    for (int i = 0; i < N; i++) {
        for (int d = 0; d < D; d++) {
            Q_q[i][d] = (gemm_data_t)(((i * 7 + d * 5 + 3) % 32) - 16);
            K_q[i][d] = (gemm_data_t)(((i * 11 + d * 13 + 1) % 32) - 16);
        }
    }
}

static int run_attention_case(int N, int D, int offset) {
    const int q_shift = 8;
    const int score_shift = 8;
    const int p_shift = ATTENTION_P_SHIFT;

    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K] = {0};
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M] = {0};
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M] = {0};
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M] = {0};

    gemm_acc_t Q_ref[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t K_ref[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t V_ref[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K] = {0};
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_data_t V_q[GEMM_MAX_K][GEMM_MAX_M] = {0};

    gemm_acc_t Score_got[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t Score_ref[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t NoSoftmax_got[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t NoSoftmax_ref[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t Attention_got[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t Attention_ref[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_data_t Score_q[GEMM_MAX_N][GEMM_MAX_K] = {0};
    gemm_data_t P_q[GEMM_MAX_N][GEMM_MAX_K] = {0};

    init_input_weights(X, Wq, Wk, Wv, N, D);

    reference_projection(X, Wq, Q_ref, N, D);
    reference_projection(X, Wk, K_ref, N, D);
    reference_projection(X, Wv, V_ref, N, D);
    reference_quantize_qkv(Q_ref, K_ref, V_ref, Q_q, K_q, V_q, N, D, q_shift);
    reference_score(Q_q, K_q, Score_ref, N, D);
    reference_quantize_score(Score_ref, Score_q, N, score_shift);
    reference_out(Score_q, V_q, NoSoftmax_ref, N, N, D);
    reference_row_normalize(Score_ref, P_q, N, p_shift);
    reference_out(P_q, V_q, Attention_ref, N, N, D);

    attention_no_softmax_top(X, Wq, Wk, Wv, NoSoftmax_got, N, D, q_shift, score_shift);
    attention_top(X, Wq, Wk, Wv, Attention_got, N, D, q_shift, p_shift);

    int max_abs_error = 0;
    long long checksum = 0;
    int errors = 0;

    errors += compare_acc("NoSoftmax", NoSoftmax_got, NoSoftmax_ref, N, D, &max_abs_error, &checksum, offset);
    errors += compare_acc("Attention", Attention_got, Attention_ref, N, D, &max_abs_error, &checksum, offset + 1000);

    std::printf("[TB] attention case N=%d D=%d mismatch_count=%d\n", N, D, errors);
    std::printf("[TB] attention case N=%d D=%d max_abs_error=%d\n", N, D, max_abs_error);
    std::printf("[TB] attention case N=%d D=%d checksum=%lld\n", N, D, checksum);

    return errors;
}

static int run_score_case(int N, int D) {
    gemm_data_t Q_q[GEMM_MAX_N][GEMM_MAX_K] = {0};
    gemm_data_t K_q[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t Score_got[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t Score_ref[GEMM_MAX_N][GEMM_MAX_M] = {0};

    init_direct_qk(Q_q, K_q, N, D);
    reference_score(Q_q, K_q, Score_ref, N, D);
    attention_score_top(Q_q, K_q, Score_got, N, D);

    int max_abs_error = 0;
    long long checksum = 0;
    int errors = compare_acc("Score", Score_got, Score_ref, N, N, &max_abs_error, &checksum, 5000);

    std::printf("[TB] score case N=%d D=%d mismatch_count=%d\n", N, D, errors);
    std::printf("[TB] score case N=%d D=%d max_abs_error=%d\n", N, D, max_abs_error);
    std::printf("[TB] score case N=%d D=%d checksum=%lld\n", N, D, checksum);
    return errors;
}

int main() {
    int errors = 0;

    errors += validate_saturate();
    errors += run_score_case(4, 4);
    errors += run_score_case(8, 8);
    errors += run_score_case(16, 96);
    errors += run_attention_case(4, 4, 0);
    errors += run_attention_case(8, 8, 10000);
    errors += run_attention_case(16, 96, 20000);

    std::printf("[TB] total_mismatch_count=%d\n", errors);
    if (errors == 0) {
        std::printf("[TB] PASS\n");
        return 0;
    }

    std::printf("[TB] FAIL, errors=%d\n", errors);
    return 1;
}
