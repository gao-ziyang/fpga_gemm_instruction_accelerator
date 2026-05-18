#include <cstdio>

#include "qkv_projection.h"

static gemm_data_t gen_x(int i, int k) {
    return (gemm_data_t)(((i * 31 + k * 17 + 5) % 128) - 64);
}

static gemm_data_t gen_w(int k, int j, int seed) {
    return (gemm_data_t)(((k * 13 + j * 29 + seed) % 128) - 64);
}

static void print_matrix(
    const char* name,
    gemm_acc_t mat[GEMM_MAX_N][GEMM_MAX_M],
    int rows,
    int cols
) {
    std::printf("%s\n", name);
    int print_rows = rows < 4 ? rows : 4;
    int print_cols = cols < 8 ? cols : 8;
    for (int i = 0; i < print_rows; i++) {
        for (int j = 0; j < print_cols; j++) {
            std::printf("%8d", (int)mat[i][j]);
        }
        if (print_cols < cols) {
            std::printf("     ...");
        }
        std::printf("\n");
    }
    if (print_rows < rows) {
        std::printf("  ...\n");
    }
}

static void golden_projection(
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

static int compare_matrix(
    const char* name,
    gemm_acc_t got[GEMM_MAX_N][GEMM_MAX_M],
    gemm_acc_t expected[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int D,
    int* max_abs_error,
    long long* checksum,
    int checksum_offset
) {
    int errors = 0;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < D; j++) {
            int diff = (int)(got[i][j] - expected[i][j]);
            int abs_diff = diff < 0 ? -diff : diff;
            if (abs_diff > *max_abs_error) {
                *max_abs_error = abs_diff;
            }
            *checksum += (long long)got[i][j] * (long long)(checksum_offset + i * D + j + 1);
            if (got[i][j] != expected[i][j]) {
                std::printf(
                    "[ERR] %s[%d][%d] got %d, expected %d\n",
                    name, i, j, (int)got[i][j], (int)expected[i][j]
                );
                errors++;
            }
        }
    }
    return errors;
}

int main() {
    const int N = 16;
    const int D = 96;

    std::printf("[TB] QKV shape: X[%d,%d] x W[%d,%d]\n", N, D, D, D);

    gemm_data_t X[GEMM_MAX_N][GEMM_MAX_K] = {0};
    gemm_data_t Wq[GEMM_MAX_K][GEMM_MAX_M] = {0};
    gemm_data_t Wk[GEMM_MAX_K][GEMM_MAX_M] = {0};
    gemm_data_t Wv[GEMM_MAX_K][GEMM_MAX_M] = {0};

    gemm_acc_t Q[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t K_out[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t V[GEMM_MAX_N][GEMM_MAX_M] = {0};

    gemm_acc_t Q_golden[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t K_golden[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t V_golden[GEMM_MAX_N][GEMM_MAX_M] = {0};

    for (int i = 0; i < N; i++) {
        for (int k = 0; k < D; k++) {
            X[i][k] = gen_x(i, k);
        }
    }

    for (int k = 0; k < D; k++) {
        for (int j = 0; j < D; j++) {
            Wq[k][j] = gen_w(k, j, 7);
            Wk[k][j] = gen_w(k, j, 19);
            Wv[k][j] = gen_w(k, j, 43);
        }
    }

    qkv_top(X, Wq, Wk, Wv, Q, K_out, V, N, D);

    golden_projection(X, Wq, Q_golden, N, D);
    golden_projection(X, Wk, K_golden, N, D);
    golden_projection(X, Wv, V_golden, N, D);

    int max_abs_error = 0;
    long long checksum = 0;
    int errors = 0;

    errors += compare_matrix("Q", Q, Q_golden, N, D, &max_abs_error, &checksum, 0);
    errors += compare_matrix("K", K_out, K_golden, N, D, &max_abs_error, &checksum, 1000);
    errors += compare_matrix("V", V, V_golden, N, D, &max_abs_error, &checksum, 2000);

    print_matrix("[TB] Q from HLS:", Q, N, D);
    print_matrix("[TB] K from HLS:", K_out, N, D);
    print_matrix("[TB] V from HLS:", V, N, D);
    std::printf("[TB] mismatch_count=%d\n", errors);
    std::printf("[TB] max_abs_error=%d\n", max_abs_error);
    std::printf("[TB] checksum=%lld\n", checksum);

    if (errors == 0) {
        std::printf("[TB] PASS\n");
        return 0;
    }

    std::printf("[TB] FAIL, errors=%d\n", errors);
    return 1;
}
