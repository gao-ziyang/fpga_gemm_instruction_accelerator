#ifndef GZY_GEMM_BENCH_TOP_H
#define GZY_GEMM_BENCH_TOP_H

#include "gemm_types.h"

static const int GEMM_BENCH_N = 16;
static const int GEMM_BENCH_K = 96;
static const int GEMM_BENCH_M = 96;

extern "C" {
void gemm_bench_top(
    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M]
);
}

#endif
