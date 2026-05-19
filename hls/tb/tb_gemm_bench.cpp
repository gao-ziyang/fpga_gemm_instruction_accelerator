#include <cstdio>

#include "gemm_bench_top.h"

static gemm_data_t gen_a(int i, int k) {
    return (gemm_data_t)(((i * 37 + k * 19 + 11) % 128) - 64);
}

static gemm_data_t gen_b(int k, int j) {
    return (gemm_data_t)(((k * 23 + j * 29 + 7) % 128) - 64);
}

int main() {
    const long long total_mac =
        (long long)GEMM_BENCH_N * (long long)GEMM_BENCH_K * (long long)GEMM_BENCH_M;

    std::printf(
        "[TB] GEMM bench: N=%d K=%d M=%d TILE=%d BLOCK_M=%d total_mac=%lld\n",
        GEMM_BENCH_N,
        GEMM_BENCH_K,
        GEMM_BENCH_M,
        GEMM_TILE,
        GEMM_BLOCK_M,
        total_mac
    );

    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K] = {0};
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M] = {0};
    gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M] = {0};
    gemm_acc_t golden[GEMM_MAX_N][GEMM_MAX_M] = {0};

    for (int i = 0; i < GEMM_BENCH_N; i++) {
        for (int k = 0; k < GEMM_BENCH_K; k++) {
            A[i][k] = gen_a(i, k);
        }
    }

    for (int k = 0; k < GEMM_BENCH_K; k++) {
        for (int j = 0; j < GEMM_BENCH_M; j++) {
            B[k][j] = gen_b(k, j);
        }
    }

    gemm_bench_top(A, B, C);

    for (int i = 0; i < GEMM_BENCH_N; i++) {
        for (int j = 0; j < GEMM_BENCH_M; j++) {
            gemm_acc_t sum = 0;
            for (int k = 0; k < GEMM_BENCH_K; k++) {
                sum += (gemm_acc_t)A[i][k] * (gemm_acc_t)B[k][j];
            }
            golden[i][j] = sum;
        }
    }

    int errors = 0;
    int max_abs_error = 0;
    long long checksum = 0;

    for (int i = 0; i < GEMM_BENCH_N; i++) {
        for (int j = 0; j < GEMM_BENCH_M; j++) {
            const int got = (int)C[i][j];
            const int ref = (int)golden[i][j];
            const int diff = got - ref;
            const int abs_diff = diff < 0 ? -diff : diff;
            if (abs_diff > max_abs_error) {
                max_abs_error = abs_diff;
            }
            checksum += (long long)got * (long long)(i * GEMM_BENCH_M + j + 1);
            if (got != ref) {
                if (errors < 16) {
                    std::printf("[ERR] C[%d][%d] got %d, expected %d\n", i, j, got, ref);
                }
                errors++;
            }
        }
    }

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
