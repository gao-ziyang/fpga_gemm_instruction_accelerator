#include <cstdio>

#include "gemm_core.h"

static gemm_data_t gen_a(int i, int k) {
    return (gemm_data_t)(((i * 37 + k * 19 + 11) % 128) - 64);
}

static gemm_data_t gen_b(int k, int j) {
    return (gemm_data_t)(((k * 23 + j * 29 + 7) % 128) - 64);
}

static void print_matrix(const char* name, gemm_acc_t mat[GEMM_MAX_N][GEMM_MAX_M], int rows, int cols) {
    std::printf("%s\n", name);
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            std::printf("%8d", (int)mat[i][j]);
        }
        std::printf("\n");
    }
}

int main() {
    const int N = 7;
    const int K = 6;
    const int M = 5;

    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K] = {0};
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M] = {0};
    gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t golden[GEMM_MAX_N][GEMM_MAX_M] = {0};

    for (int i = 0; i < N; i++) {
        for (int k = 0; k < K; k++) {
            A[i][k] = gen_a(i, k);
        }
    }

    for (int k = 0; k < K; k++) {
        for (int j = 0; j < M; j++) {
            B[k][j] = gen_b(k, j);
        }
    }

    gemm_top(A, B, C, N, K, M, true);

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < M; j++) {
            gemm_acc_t sum = 0;
            for (int k = 0; k < K; k++) {
                sum += (gemm_acc_t)A[i][k] * (gemm_acc_t)B[k][j];
            }
            golden[i][j] = sum >> GEMM_OUT_SHIFT;
        }
    }

    int errors = 0;
    int max_abs_error = 0;
    long long checksum = 0;

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < M; j++) {
            int diff = (int)(C[i][j] - golden[i][j]);
            int abs_diff = diff < 0 ? -diff : diff;
            if (abs_diff > max_abs_error) {
                max_abs_error = abs_diff;
            }
            checksum += (long long)C[i][j] * (long long)(i * M + j + 1);
            if (C[i][j] != golden[i][j]) {
                std::printf(
                    "[ERR] C[%d][%d] got %d, expected %d\n",
                    i, j, (int)C[i][j], (int)golden[i][j]
                );
                errors++;
            }
        }
    }

    print_matrix("[TB] C from HLS:", C, N, M);
    print_matrix("[TB] Golden:", golden, N, M);
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
