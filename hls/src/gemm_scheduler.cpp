#include "gemm_scheduler.h"

#include "gemm_core.h"

#ifndef GZY_ACCEL_LOAD_AB_PARALLEL
#define GZY_ACCEL_LOAD_AB_PARALLEL 0
#endif

#ifndef GZY_ACCEL_LOCAL_ROW_UNROLL
#define GZY_ACCEL_LOCAL_ROW_UNROLL 1
#endif

#ifndef GZY_ACCEL_LOCAL_AB_PARALLEL
#define GZY_ACCEL_LOCAL_AB_PARALLEL 0
#endif

#ifndef GZY_ACCEL_LOCAL_AB_HELPER_INLINE
#define GZY_ACCEL_LOCAL_AB_HELPER_INLINE 0
#endif

#ifndef GZY_ACCEL_LOCAL_AB_HELPER_PARTITION
#define GZY_ACCEL_LOCAL_AB_HELPER_PARTITION 1
#endif

#ifndef GZY_ACCEL_LOCAL_AB_DIRECT
#define GZY_ACCEL_LOCAL_AB_DIRECT 0
#endif

#ifndef GZY_ACCEL_LOCAL_DOUBLE_BUFFER
#define GZY_ACCEL_LOCAL_DOUBLE_BUFFER 0
#endif

#ifndef GZY_ACCEL_DATAFLOW_BLOCK_OVERLAP
#define GZY_ACCEL_DATAFLOW_BLOCK_OVERLAP 0
#endif

#ifndef GZY_ACCEL_COMPUTE_PADDED_INPUTS
#define GZY_ACCEL_COMPUTE_PADDED_INPUTS 0
#endif

#ifndef GZY_ACCEL_EXPLICIT_BANKS
#define GZY_ACCEL_EXPLICIT_BANKS 0
#endif

#ifndef GZY_ACCEL_FULL_ONLY
#define GZY_ACCEL_FULL_ONLY 0//0则考虑边界，默认有边界判断电路等；1则不考虑边界，综合出来的电路更简单更快，但只能处理NKM都是block size整数倍的情况。
#endif


//不考虑边界的宏定义，O6ab优化版本。有compute_block和compute_block_full两个函数。
//但是效果不好，综合出来了两套硬件，且因为我NKM测试比较小所以没发挥出效果。
/*
if 当前是完整 block:
    走 full path
else:
    走 generic path
    */
#ifndef GZY_ACCEL_FULL_BLOCK_FAST
#define GZY_ACCEL_FULL_BLOCK_FAST 0
#endif

#ifndef GZY_ACCEL_SHARED_GEMM_INLINE
#define GZY_ACCEL_SHARED_GEMM_INLINE 0
#endif

#ifndef GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD
#define GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD 0
#endif

#if GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD && GZY_ACCEL_LOAD_AB_PARALLEL
#error "GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD currently requires GZY_ACCEL_LOAD_AB_PARALLEL=0"
#endif

#if GZY_ACCEL_FULL_ONLY
#if (GZY_ACCEL_BENCH_N % GZY_ACCEL_BLOCK_N) != 0
#error "GZY_ACCEL_FULL_ONLY requires GZY_ACCEL_BENCH_N to be a multiple of GZY_ACCEL_BLOCK_N"
#endif
#if (GZY_ACCEL_BENCH_K % GZY_ACCEL_BLOCK_K) != 0
#error "GZY_ACCEL_FULL_ONLY requires GZY_ACCEL_BENCH_K to be a multiple of GZY_ACCEL_BLOCK_K"
#endif
#if (GZY_ACCEL_BENCH_M % GZY_ACCEL_BLOCK_M) != 0
#error "GZY_ACCEL_FULL_ONLY requires GZY_ACCEL_BENCH_M to be a multiple of GZY_ACCEL_BLOCK_M"
#endif
#endif

#if GZY_ACCEL_COMPUTE_PADDED_INPUTS
#if (GZY_ACCEL_BLOCK_N % GZY_GEMM_TILE) != 0
#error "GZY_ACCEL_COMPUTE_PADDED_INPUTS requires BLOCK_N to be a multiple of TILE"
#endif
#if (GZY_ACCEL_BLOCK_K % GZY_GEMM_TILE) != 0
#error "GZY_ACCEL_COMPUTE_PADDED_INPUTS requires BLOCK_K to be a multiple of TILE"
#endif
#if (GZY_ACCEL_BLOCK_M % GZY_GEMM_TILE) != 0
#error "GZY_ACCEL_COMPUTE_PADDED_INPUTS requires BLOCK_M to be a multiple of TILE"
#endif
#endif

#if GZY_ACCEL_EXPLICIT_BANKS
#if !GZY_ACCEL_COMPUTE_PADDED_INPUTS
#error "GZY_ACCEL_EXPLICIT_BANKS requires GZY_ACCEL_COMPUTE_PADDED_INPUTS"
#endif
#if GZY_ACCEL_FULL_ONLY
#error "GZY_ACCEL_EXPLICIT_BANKS is only implemented for the generic scheduler path"
#endif
#if GZY_ACCEL_FULL_BLOCK_FAST
#error "GZY_ACCEL_EXPLICIT_BANKS is only implemented with FULL_BLOCK_FAST=0"
#endif
#if GZY_ACCEL_LOCAL_DOUBLE_BUFFER
#error "GZY_ACCEL_EXPLICIT_BANKS is only implemented with LOCAL_DOUBLE_BUFFER=0"
#endif
#if GZY_ACCEL_LOCAL_AB_PARALLEL
#error "GZY_ACCEL_EXPLICIT_BANKS is only implemented with LOCAL_AB_PARALLEL=0"
#endif
#if GZY_ACCEL_LOCAL_AB_DIRECT
#error "GZY_ACCEL_EXPLICIT_BANKS is only implemented with LOCAL_AB_DIRECT=0"
#endif
#if !GZY_ACCEL_LOAD_AB_PARALLEL
#error "GZY_ACCEL_EXPLICIT_BANKS is only implemented with LOAD_AB_PARALLEL=1"
#endif
#endif

static const int ACCEL_LOCAL_ROW_UNROLL = GZY_ACCEL_LOCAL_ROW_UNROLL;
static const int ACCEL_LOAD_AB_COLS =
    (ACCEL_BLOCK_N > ACCEL_BLOCK_M) ? ACCEL_BLOCK_N : ACCEL_BLOCK_M;
static const int ACCEL_BLOCK_K_TILE_COUNT =
    (ACCEL_BLOCK_K + GEMM_TILE - 1) / GEMM_TILE;
static const int ACCEL_BLOCK_K_GROUPS = ACCEL_BLOCK_K / GEMM_TILE;
static const int ACCEL_BLOCK_M_GROUPS = ACCEL_BLOCK_M / GEMM_TILE;

static int min_int(int a, int b) {
#pragma HLS INLINE
    return (a < b) ? a : b;
}

static gemm_data_t scheduler_quantize_acc_to_i8(gemm_acc_t value, int shift) {
#pragma HLS INLINE
    gemm_acc_t shifted = value;
    if (shift > 0) {
        shifted = value >> shift;
    }

    if (shifted > 127) {
        return (gemm_data_t)127;
    }
    if (shifted < -128) {
        return (gemm_data_t)-128;
    }
    return (gemm_data_t)shifted;
}

static void load_a_block(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    int N,
    int K,
    int n0,
    int k0,
    int a_base,
    int current_N,
    int current_K
) {
#pragma HLS INLINE off
load_a_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    load_a_k:
        for (int k = 0; k < ACCEL_BLOCK_K; k++) {
#pragma HLS PIPELINE II=1
            if (i < current_N && k < current_K) {
                A_buf[i][k] = A_mem[a_base + (n0 + i) * K + (k0 + k)];
            } else {
                A_buf[i][k] = 0;
            }
        }
    }
}

static void load_b_block(
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    int K,
    int M,
    int k0,
    int m0,
    int b_base,
    int current_K,
    int current_M
) {
#pragma HLS INLINE off
load_b_k:
    for (int k = 0; k < ACCEL_BLOCK_K; k++) {
    load_b_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            if (k < current_K && j < current_M) {
                B_buf[k][j] = B_mem[b_base + (k0 + k) * M + (m0 + j)];
            } else {
                B_buf[k][j] = 0;
            }
        }
    }
}

static void load_b_conv_weight_block(
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    int M,
    int k0,
    int m0,
    int b_base,
    int current_K,
    int current_M,
    int conv_cin,
    int conv_kh,
    int conv_kw
) {
#pragma HLS INLINE off
    const int khkw = conv_kh * conv_kw;
load_b_conv_k:
    for (int k = 0; k < ACCEL_BLOCK_K; k++) {
    load_b_conv_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            if (k < current_K && j < current_M) {
                const int flat_k = k0 + k;
                const int ci = flat_k / khkw;
                const int rem = flat_k - ci * khkw;
                const int r = rem / conv_kw;
                const int s = rem - r * conv_kw;
                const int co = m0 + j;
                B_buf[k][j] =
                    B_mem[b_base + ((co * conv_cin + ci) * conv_kh + r) * conv_kw + s];
            } else {
                B_buf[k][j] = 0;
            }
        }
    }
}

static void load_ab_block(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    int N,
    int K,
    int M,
    int n0,
    int k0,
    int m0,
    int a_base,
    int b_base,
    int current_N,
    int current_K,
    int current_M
) {
#pragma HLS INLINE off
load_ab_k:
    for (int k = 0; k < ACCEL_BLOCK_K; k++) {
    load_ab_x:
        for (int x = 0; x < ACCEL_LOAD_AB_COLS; x++) {
#pragma HLS PIPELINE II=1
            if (x < ACCEL_BLOCK_N) {
                if (x < current_N && k < current_K) {
                    A_buf[x][k] = A_mem[a_base + (n0 + x) * K + (k0 + k)];
                } else {
                    A_buf[x][k] = 0;
                }
            }

            if (x < ACCEL_BLOCK_M) {
                if (k < current_K && x < current_M) {
                    B_buf[k][x] = B_mem[b_base + (k0 + k) * M + (m0 + x)];
                } else {
                    B_buf[k][x] = 0;
                }
            }
        }
    }
}

static void load_a_block_full(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    int N,
    int K,
    int n0,
    int k0,
    int a_base
) {
#pragma HLS INLINE off
load_a_full_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    load_a_full_k:
        for (int k = 0; k < ACCEL_BLOCK_K; k++) {
#pragma HLS PIPELINE II=1
            A_buf[i][k] = A_mem[a_base + (n0 + i) * K + (k0 + k)];
        }
    }
}

static void load_b_block_full(
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    int K,
    int M,
    int k0,
    int m0,
    int b_base
) {
#pragma HLS INLINE off
load_b_full_k:
    for (int k = 0; k < ACCEL_BLOCK_K; k++) {
    load_b_full_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            B_buf[k][j] = B_mem[b_base + (k0 + k) * M + (m0 + j)];
        }
    }
}

static void load_ab_block_full(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    int N,
    int K,
    int M,
    int n0,
    int k0,
    int m0,
    int a_base,
    int b_base
) {
#pragma HLS INLINE off
#if GZY_ACCEL_BLOCK_N == GZY_ACCEL_BLOCK_M
load_ab_full_equal_k:
    for (int k = 0; k < ACCEL_BLOCK_K; k++) {
    load_ab_full_equal_x:
        for (int x = 0; x < ACCEL_BLOCK_N; x++) {
#pragma HLS PIPELINE II=1
            A_buf[x][k] = A_mem[a_base + (n0 + x) * K + (k0 + k)];
            B_buf[k][x] = B_mem[b_base + (k0 + k) * M + (m0 + x)];
        }
    }
#else
load_ab_full_k:
    for (int k = 0; k < ACCEL_BLOCK_K; k++) {
    load_ab_full_x:
        for (int x = 0; x < ACCEL_LOAD_AB_COLS; x++) {
#pragma HLS PIPELINE II=1
            if (x < ACCEL_BLOCK_N) {
                A_buf[x][k] = A_mem[a_base + (n0 + x) * K + (k0 + k)];
            }
            if (x < ACCEL_BLOCK_M) {
                B_buf[k][x] = B_mem[b_base + (k0 + k) * M + (m0 + x)];
            }
        }
    }
#endif
}

#if GZY_ACCEL_EXPLICIT_BANKS
static void load_ab_block_banked(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_data_t A_bank[GEMM_TILE][ACCEL_BLOCK_N][ACCEL_BLOCK_K_GROUPS],
    gemm_data_t B_bank[GEMM_TILE][ACCEL_BLOCK_K][ACCEL_BLOCK_M_GROUPS],
    int N,
    int K,
    int M,
    int n0,
    int k0,
    int m0,
    int a_base,
    int b_base,
    int current_N,
    int current_K,
    int current_M
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=A_bank complete dim=1
#pragma HLS ARRAY_PARTITION variable=B_bank complete dim=1
load_ab_bank_kg:
    for (int kg = 0; kg < ACCEL_BLOCK_K_GROUPS; kg++) {
    load_ab_bank_kb:
        for (int kb = 0; kb < GEMM_TILE; kb++) {
            const int k = kg * GEMM_TILE + kb;
        load_ab_bank_x:
            for (int x = 0; x < ACCEL_LOAD_AB_COLS; x++) {
#pragma HLS PIPELINE II=1
                if (x < ACCEL_BLOCK_N) {
                    if (x < current_N && k < current_K) {
                        A_bank[kb][x][kg] =
                            A_mem[a_base + (n0 + x) * K + (k0 + k)];
                    } else {
                        A_bank[kb][x][kg] = 0;
                    }
                }

                if (x < ACCEL_BLOCK_M) {
                    const int jb = x % GEMM_TILE;
                    const int jg = x / GEMM_TILE;
                    if (k < current_K && x < current_M) {
                        B_bank[jb][k][jg] =
                            B_mem[b_base + (k0 + k) * M + (m0 + x)];
                    } else {
                        B_bank[jb][k][jg] = 0;
                    }
                }
            }
        }
    }
}
#endif

static void load_local_ab_tile(
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    gemm_data_t localA[GEMM_TILE][GEMM_TILE],
    gemm_data_t localB[GEMM_TILE][GEMM_TILE],
    int ti,
    int tj,
    int tk,
    int current_N,
    int current_K,
    int current_M
) {
#if GZY_ACCEL_LOCAL_AB_HELPER_INLINE
#pragma HLS INLINE
#else
#pragma HLS INLINE off
#endif
#if GZY_ACCEL_LOCAL_AB_HELPER_PARTITION
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=GZY_GEMM_TILE dim=2
#endif
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0
load_local_ab_row:
    for (int r = 0; r < GEMM_TILE; r++) {
#pragma HLS PIPELINE II=1
    load_local_ab_col:
        for (int x = 0; x < GEMM_TILE; x++) {
#pragma HLS UNROLL
            const int ai = ti + r;
            const int ak = tk + x;
            if (ai < current_N && ak < current_K) {
                localA[r][x] = A_buf[ai][ak];
            } else {
                localA[r][x] = 0;
            }

            const int bk = tk + r;
            const int bj = tj + x;
            if (bk < current_K && bj < current_M) {
                localB[r][x] = B_buf[bk][bj];
            } else {
                localB[r][x] = 0;
            }
        }
    }
}

static void load_local_a_bank(
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t localA_bank[2][GEMM_TILE][GEMM_TILE],
    int bank,
    int ti,
    int tk,
    int current_N,
    int current_K
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=localA_bank complete dim=0
load_local_a_db_group:
    for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
    load_local_a_db_u:
        for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
            const int ii = ii0 + u;
        load_local_a_db_k:
            for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS UNROLL
                const int bi = ti + ii;
                const int bk = tk + kk;
                if (ii < GEMM_TILE && bi < current_N && bk < current_K) {
                    localA_bank[bank][ii][kk] = A_buf[bi][bk];
                } else if (ii < GEMM_TILE) {
                    localA_bank[bank][ii][kk] = 0;
                }
            }
        }
    }
}

static void load_local_b_bank(
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    gemm_data_t localB_bank[2][GEMM_TILE][GEMM_TILE],
    int bank,
    int tj,
    int tk,
    int current_K,
    int current_M
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=localB_bank complete dim=0
load_local_b_db_group:
    for (int kk0 = 0; kk0 < GEMM_TILE; kk0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
    load_local_b_db_u:
        for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
            const int kk = kk0 + u;
        load_local_b_db_j:
            for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                const int bk = tk + kk;
                const int bj = tj + jj;
                if (kk < GEMM_TILE && bk < current_K && bj < current_M) {
                    localB_bank[bank][kk][jj] = B_buf[bk][bj];
                } else if (kk < GEMM_TILE) {
                    localB_bank[bank][kk][jj] = 0;
                }
            }
        }
    }
}

static void load_local_a_tile_static(
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t localA[GEMM_TILE][GEMM_TILE],
    int ti,
    int tk,
    int current_N,
    int current_K
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
load_local_a_static_group:
    for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
    load_local_a_static_u:
        for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
            const int ii = ii0 + u;
        load_local_a_static_k:
            for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS UNROLL
                const int bi = ti + ii;
                const int bk = tk + kk;
                if (ii < GEMM_TILE && bi < current_N && bk < current_K) {
                    localA[ii][kk] = A_buf[bi][bk];
                } else if (ii < GEMM_TILE) {
                    localA[ii][kk] = 0;
                }
            }
        }
    }
}

static void load_local_b_tile_static(
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    gemm_data_t localB[GEMM_TILE][GEMM_TILE],
    int tj,
    int tk,
    int current_K,
    int current_M
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0
load_local_b_static_group:
    for (int kk0 = 0; kk0 < GEMM_TILE; kk0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
    load_local_b_static_u:
        for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
            const int kk = kk0 + u;
        load_local_b_static_j:
            for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                const int bk = tk + kk;
                const int bj = tj + jj;
                if (kk < GEMM_TILE && bk < current_K && bj < current_M) {
                    localB[kk][jj] = B_buf[bk][bj];
                } else if (kk < GEMM_TILE) {
                    localB[kk][jj] = 0;
                }
            }
        }
    }
}

static void load_local_a_tile_kt(
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t localA[GEMM_TILE][GEMM_TILE],
    int ti,
    int ktile,
    int current_N,
    int current_K
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
load_local_a_kt_group:
    for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
    load_local_a_kt_u:
        for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
            const int ii = ii0 + u;
        load_local_a_kt_k:
            for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS UNROLL
                const int bi = ti + ii;
                const int bk = ktile * GEMM_TILE + kk;
                if (ii < GEMM_TILE && bi < current_N && bk < current_K) {
                    localA[ii][kk] = A_buf[bi][bk];
                } else if (ii < GEMM_TILE) {
                    localA[ii][kk] = 0;
                }
            }
        }
    }
}

static void load_local_b_tile_kt(
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    gemm_data_t localB[GEMM_TILE][GEMM_TILE],
    int tj,
    int ktile,
    int current_K,
    int current_M
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0
load_local_b_kt_group:
    for (int kk0 = 0; kk0 < GEMM_TILE; kk0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
    load_local_b_kt_u:
        for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
            const int kk = kk0 + u;
        load_local_b_kt_j:
            for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                const int bk = ktile * GEMM_TILE + kk;
                const int bj = tj + jj;
                if (kk < GEMM_TILE && bk < current_K && bj < current_M) {
                    localB[kk][jj] = B_buf[bk][bj];
                } else if (kk < GEMM_TILE) {
                    localB[kk][jj] = 0;
                }
            }
        }
    }
}

static void load_next_and_compute_kt(
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    gemm_data_t localA_next[GEMM_TILE][GEMM_TILE],
    gemm_data_t localB_next[GEMM_TILE][GEMM_TILE],
    gemm_data_t localA_curr[GEMM_TILE][GEMM_TILE],
    gemm_data_t localB_curr[GEMM_TILE][GEMM_TILE],
    gemm_acc_t localC[GEMM_TILE][GEMM_TILE],
    int ti,
    int tj,
    int next_ktile,
    int current_N,
    int current_K,
    int current_M
) {
#pragma HLS INLINE off
#pragma HLS DATAFLOW
#pragma HLS ARRAY_PARTITION variable=localA_next complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB_next complete dim=0
#pragma HLS ARRAY_PARTITION variable=localA_curr complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB_curr complete dim=0
#pragma HLS ARRAY_PARTITION variable=localC complete dim=0
    load_local_a_tile_kt(A_buf, localA_next, ti, next_ktile, current_N, current_K);
    load_local_b_tile_kt(B_buf, localB_next, tj, next_ktile, current_K, current_M);
    gemm_core_mac(localA_curr, localB_curr, localC);
}

static void compute_block_full(
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M],
    bool reset_c
) {
#pragma HLS INLINE off
compute_full_tile_i:
    for (int ti = 0; ti < ACCEL_BLOCK_N; ti += GEMM_TILE) {
    compute_full_tile_j:
        for (int tj = 0; tj < ACCEL_BLOCK_M; tj += GEMM_TILE) {
            gemm_acc_t localC[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localC complete dim=0

        load_full_local_c_group:
            for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
            load_full_local_c_u:
                for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                    const int ii = ii0 + u;
                load_full_local_c_j:
                    for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                        if (ii < GEMM_TILE) {
                            localC[ii][jj] = reset_c ? (gemm_acc_t)0 : C_buf[ti + ii][tj + jj];
                        }
                    }
                }
            }

        compute_full_tile_k:
            for (int tk = 0; tk < ACCEL_BLOCK_K; tk += GEMM_TILE) {
                gemm_data_t localA[GEMM_TILE][GEMM_TILE];
                gemm_data_t localB[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0

            load_full_local_a_group:
                for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
                load_full_local_a_u:
                    for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                        const int ii = ii0 + u;
                    load_full_local_a_k:
                        for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS UNROLL
                            if (ii < GEMM_TILE) {
                                localA[ii][kk] = A_buf[ti + ii][tk + kk];
                            }
                        }
                    }
                }

            load_full_local_b_group:
                for (int kk0 = 0; kk0 < GEMM_TILE; kk0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
                load_full_local_b_u:
                    for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                        const int kk = kk0 + u;
                    load_full_local_b_j:
                        for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                            if (kk < GEMM_TILE) {
                                localB[kk][jj] = B_buf[tk + kk][tj + jj];
                            }
                        }
                    }
                }

                gemm_core_mac(localA, localB, localC);
            }

        store_full_local_c_group:
            for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
            store_full_local_c_u:
                for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                    const int ii = ii0 + u;
                store_full_local_c_j:
                    for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                        if (ii < GEMM_TILE) {
                            C_buf[ti + ii][tj + jj] = localC[ii][jj];
                        }
                    }
                }
            }
        }
    }
}

static void compute_block(
    gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K],
    gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M],
    gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M],
    int current_N,
    int current_K,
    int current_M,
    bool reset_c
) {
#pragma HLS INLINE off
#if GZY_ACCEL_LOCAL_DOUBLE_BUFFER == 2
#pragma HLS ALLOCATION function instances=gemm_core_mac limit=1
#pragma HLS ALLOCATION function instances=load_local_a_tile_static limit=1
#pragma HLS ALLOCATION function instances=load_local_b_tile_static limit=1
#endif
compute_tile_i:
    for (int ti = 0; ti < ACCEL_BLOCK_N; ti += GEMM_TILE) {
    compute_tile_j:
        for (int tj = 0; tj < ACCEL_BLOCK_M; tj += GEMM_TILE) {
            gemm_acc_t localC[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localC complete dim=0

        load_local_c_group:
            for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
            load_local_c_u:
                for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                    const int ii = ii0 + u;
                load_local_c_j:
                    for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                        int bi = ti + ii;
                        int bj = tj + jj;
#if GZY_ACCEL_COMPUTE_PADDED_INPUTS
                        if (ii < GEMM_TILE) {
                            localC[ii][jj] = reset_c ? (gemm_acc_t)0 : C_buf[bi][bj];
                        }
#else
                        if (ii >= GEMM_TILE || reset_c) {
                            if (ii < GEMM_TILE) {
                                localC[ii][jj] = 0;
                            }
                        } else if (bi < current_N && bj < current_M) {
                            localC[ii][jj] = C_buf[bi][bj];
                        } else {
                            localC[ii][jj] = 0;
                        }
#endif
                    }
                }
            }

#if GZY_ACCEL_LOCAL_DOUBLE_BUFFER == 3
            gemm_data_t localA_ping[GEMM_TILE][GEMM_TILE];
            gemm_data_t localB_ping[GEMM_TILE][GEMM_TILE];
            gemm_data_t localA_pong[GEMM_TILE][GEMM_TILE];
            gemm_data_t localB_pong[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localA_ping complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB_ping complete dim=0
#pragma HLS ARRAY_PARTITION variable=localA_pong complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB_pong complete dim=0

            load_local_a_tile_kt(A_buf, localA_ping, ti, 0, current_N, current_K);
            load_local_b_tile_kt(B_buf, localB_ping, tj, 0, current_K, current_M);

        compute_tile_k_db_kt:
            for (int ktile = 0; ktile < ACCEL_BLOCK_K_TILE_COUNT; ktile += 2) {
                load_next_and_compute_kt(
                    A_buf,
                    B_buf,
                    localA_pong,
                    localB_pong,
                    localA_ping,
                    localB_ping,
                    localC,
                    ti,
                    tj,
                    ktile + 1,
                    current_N,
                    current_K,
                    current_M
                );

                if (ktile + 1 < ACCEL_BLOCK_K_TILE_COUNT) {
                    load_next_and_compute_kt(
                        A_buf,
                        B_buf,
                        localA_ping,
                        localB_ping,
                        localA_pong,
                        localB_pong,
                        localC,
                        ti,
                        tj,
                        ktile + 2,
                        current_N,
                        current_K,
                        current_M
                    );
                }
            }
#elif GZY_ACCEL_LOCAL_DOUBLE_BUFFER == 2
            gemm_data_t localA_ping[GEMM_TILE][GEMM_TILE];
            gemm_data_t localB_ping[GEMM_TILE][GEMM_TILE];
            gemm_data_t localA_pong[GEMM_TILE][GEMM_TILE];
            gemm_data_t localB_pong[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localA_ping complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB_ping complete dim=0
#pragma HLS ARRAY_PARTITION variable=localA_pong complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB_pong complete dim=0

            load_local_a_tile_static(A_buf, localA_ping, ti, 0, current_N, current_K);
            load_local_b_tile_static(B_buf, localB_ping, tj, 0, current_K, current_M);

        compute_tile_k_db_static:
            for (int tk = 0; tk < ACCEL_BLOCK_K; tk += 2 * GEMM_TILE) {
                const int tk_pong = tk + GEMM_TILE;
                const int tk_next_ping = tk + 2 * GEMM_TILE;

                if (tk_pong < ACCEL_BLOCK_K) {
                    {
#pragma HLS DATAFLOW
                        load_local_a_tile_static(A_buf, localA_pong, ti, tk_pong, current_N, current_K);
                        load_local_b_tile_static(B_buf, localB_pong, tj, tk_pong, current_K, current_M);
                        gemm_core_mac(localA_ping, localB_ping, localC);
                    }
                } else {
                    gemm_core_mac(localA_ping, localB_ping, localC);
                }

                if (tk_next_ping < ACCEL_BLOCK_K) {
                    {
#pragma HLS DATAFLOW
                        load_local_a_tile_static(A_buf, localA_ping, ti, tk_next_ping, current_N, current_K);
                        load_local_b_tile_static(B_buf, localB_ping, tj, tk_next_ping, current_K, current_M);
                        gemm_core_mac(localA_pong, localB_pong, localC);
                    }
                } else if (tk_pong < ACCEL_BLOCK_K) {
                    gemm_core_mac(localA_pong, localB_pong, localC);
                }
            }
#elif GZY_ACCEL_LOCAL_DOUBLE_BUFFER
            gemm_data_t localA_bank[2][GEMM_TILE][GEMM_TILE];
            gemm_data_t localB_bank[2][GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localA_bank complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB_bank complete dim=0

            load_local_a_bank(A_buf, localA_bank, 0, ti, 0, current_N, current_K);
            load_local_b_bank(B_buf, localB_bank, 0, tj, 0, current_K, current_M);

        compute_tile_k_db:
            for (int tk = 0; tk < ACCEL_BLOCK_K; tk += GEMM_TILE) {
                const int bank = (tk / GEMM_TILE) & 1;
                const int next_bank = 1 - bank;
                const int next_tk = tk + GEMM_TILE;

                if (next_tk < ACCEL_BLOCK_K) {
                    load_local_a_bank(A_buf, localA_bank, next_bank, ti, next_tk, current_N, current_K);
                    load_local_b_bank(B_buf, localB_bank, next_bank, tj, next_tk, current_K, current_M);
                }

                gemm_core_mac(localA_bank[bank], localB_bank[bank], localC);
            }
#else
        compute_tile_k:
            for (int tk = 0; tk < ACCEL_BLOCK_K; tk += GEMM_TILE) {
                gemm_data_t localA[GEMM_TILE][GEMM_TILE];
                gemm_data_t localB[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0

#if GZY_ACCEL_LOCAL_AB_DIRECT
            load_local_ab_direct_row:
                for (int r = 0; r < GEMM_TILE; r++) {
#pragma HLS PIPELINE II=1
                load_local_ab_direct_col:
                    for (int x = 0; x < GEMM_TILE; x++) {
#pragma HLS UNROLL
                        const int ai = ti + r;
                        const int ak = tk + x;
                        if (ai < current_N && ak < current_K) {
                            localA[r][x] = A_buf[ai][ak];
                        } else {
                            localA[r][x] = 0;
                        }

                        const int bk = tk + r;
                        const int bj = tj + x;
                        if (bk < current_K && bj < current_M) {
                            localB[r][x] = B_buf[bk][bj];
                        } else {
                            localB[r][x] = 0;
                        }
                    }
                }
#elif GZY_ACCEL_LOCAL_AB_PARALLEL
                load_local_ab_tile(
                    A_buf,
                    B_buf,
                    localA,
                    localB,
                    ti,
                    tj,
                    tk,
                    current_N,
                    current_K,
                    current_M
                );
#else
            load_local_a_group:
                for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
                load_local_a_u:
                    for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                        const int ii = ii0 + u;
                    load_local_a_k:
                        for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS UNROLL
                            int bi = ti + ii;
                            int bk = tk + kk;
#if GZY_ACCEL_COMPUTE_PADDED_INPUTS
                            if (ii < GEMM_TILE) {
                                localA[ii][kk] = A_buf[bi][bk];
                            }
#else
                            if (ii < GEMM_TILE && bi < current_N && bk < current_K) {
                                localA[ii][kk] = A_buf[bi][bk];
                            } else if (ii < GEMM_TILE) {
                                localA[ii][kk] = 0;
                            }
#endif
                        }
                    }
                }

            load_local_b_group:
                for (int kk0 = 0; kk0 < GEMM_TILE; kk0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
                load_local_b_u:
                    for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                        const int kk = kk0 + u;
                    load_local_b_j:
                        for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                            int bk = tk + kk;
                            int bj = tj + jj;
#if GZY_ACCEL_COMPUTE_PADDED_INPUTS
                            if (kk < GEMM_TILE) {
                                localB[kk][jj] = B_buf[bk][bj];
                            }
#else
                            if (kk < GEMM_TILE && bk < current_K && bj < current_M) {
                                localB[kk][jj] = B_buf[bk][bj];
                            } else if (kk < GEMM_TILE) {
                                localB[kk][jj] = 0;
                            }
#endif
                        }
                    }
                }
#endif

                gemm_core_mac(localA, localB, localC);
            }
#endif

        store_local_c_group:
            for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
            store_local_c_u:
                for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                    const int ii = ii0 + u;
                store_local_c_j:
                    for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                        int bi = ti + ii;
                        int bj = tj + jj;
#if GZY_ACCEL_COMPUTE_PADDED_INPUTS
                        if (ii < GEMM_TILE) {
                            C_buf[bi][bj] = localC[ii][jj];
                        }
#else
                        if (ii < GEMM_TILE && bi < current_N && bj < current_M) {
                            C_buf[bi][bj] = localC[ii][jj];
                        }
#endif
                    }
                }
            }
        }
    }
}

#if GZY_ACCEL_EXPLICIT_BANKS
static void compute_block_banked(
    gemm_data_t A_bank[GEMM_TILE][ACCEL_BLOCK_N][ACCEL_BLOCK_K_GROUPS],
    gemm_data_t B_bank[GEMM_TILE][ACCEL_BLOCK_K][ACCEL_BLOCK_M_GROUPS],
    gemm_acc_t C_bank[GEMM_TILE][ACCEL_BLOCK_N][ACCEL_BLOCK_M_GROUPS],
    bool reset_c
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=A_bank complete dim=1
#pragma HLS ARRAY_PARTITION variable=B_bank complete dim=1
#pragma HLS ARRAY_PARTITION variable=C_bank complete dim=1
compute_banked_tile_i:
    for (int ti = 0; ti < ACCEL_BLOCK_N; ti += GEMM_TILE) {
    compute_banked_tile_j:
        for (int tj = 0; tj < ACCEL_BLOCK_M; tj += GEMM_TILE) {
            const int jg = tj / GEMM_TILE;
            gemm_acc_t localC[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localC complete dim=0

        load_banked_local_c_group:
            for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
            load_banked_local_c_u:
                for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                    const int ii = ii0 + u;
                load_banked_local_c_j:
                    for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                        if (ii < GEMM_TILE) {
                            localC[ii][jj] =
                                reset_c ? (gemm_acc_t)0 : C_bank[jj][ti + ii][jg];
                        }
                    }
                }
            }

        compute_banked_tile_k:
            for (int tk = 0; tk < ACCEL_BLOCK_K; tk += GEMM_TILE) {
                const int kg = tk / GEMM_TILE;
                gemm_data_t localA[GEMM_TILE][GEMM_TILE];
                gemm_data_t localB[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0

            load_banked_local_a_group:
                for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
                load_banked_local_a_u:
                    for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                        const int ii = ii0 + u;
                    load_banked_local_a_k:
                        for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS UNROLL
                            if (ii < GEMM_TILE) {
                                localA[ii][kk] = A_bank[kk][ti + ii][kg];
                            }
                        }
                    }
                }

            load_banked_local_b_group:
                for (int kk0 = 0; kk0 < GEMM_TILE; kk0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
                load_banked_local_b_u:
                    for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                        const int kk = kk0 + u;
                    load_banked_local_b_j:
                        for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                            if (kk < GEMM_TILE) {
                                localB[kk][jj] = B_bank[jj][tk + kk][jg];
                            }
                        }
                    }
                }

                gemm_core_mac(localA, localB, localC);
            }

        store_banked_local_c_group:
            for (int ii0 = 0; ii0 < GEMM_TILE; ii0 += ACCEL_LOCAL_ROW_UNROLL) {
#pragma HLS PIPELINE II=1
            store_banked_local_c_u:
                for (int u = 0; u < ACCEL_LOCAL_ROW_UNROLL; u++) {
#pragma HLS UNROLL
                    const int ii = ii0 + u;
                store_banked_local_c_j:
                    for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                        if (ii < GEMM_TILE) {
                            C_bank[jj][ti + ii][jg] = localC[ii][jj];
                        }
                    }
                }
            }
        }
    }
}
#endif

static void store_c_block_full(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M],
    int N,
    int M,
    int n0,
    int m0,
    int c_base
) {
#pragma HLS INLINE off
store_c_full_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    store_c_full_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            C_mem[c_base + (n0 + i) * M + (m0 + j)] = C_buf[i][j];
        }
    }
}

static void store_c_block(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M],
    int N,
    int M,
    int n0,
    int m0,
    int c_base,
    int current_N,
    int current_M
) {
#pragma HLS INLINE off
store_c_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    store_c_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            if (i < current_N && j < current_M) {
                C_mem[c_base + (n0 + i) * M + (m0 + j)] = C_buf[i][j];
            }
        }
    }
}

static void store_c_block_transpose(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M],
    int N,
    int n0,
    int m0,
    int c_base,
    int current_N,
    int current_M
) {
#pragma HLS INLINE off
store_c_t_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    store_c_t_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            if (i < current_N && j < current_M) {
                C_mem[c_base + (m0 + j) * N + (n0 + i)] = C_buf[i][j];
            }
        }
    }
}

static void store_i8_a_block(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M],
    int M,
    int n0,
    int m0,
    int out_base,
    int shift,
    int current_N,
    int current_M
) {
#pragma HLS INLINE off
store_i8_a_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    store_i8_a_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            if (i < current_N && j < current_M) {
                A_mem[out_base + (n0 + i) * M + (m0 + j)] =
                    scheduler_quantize_acc_to_i8(C_buf[i][j], shift);
            }
        }
    }
}

static void store_i8_b_block(
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M],
    int M,
    int n0,
    int m0,
    int out_base,
    int shift,
    int current_N,
    int current_M
) {
#pragma HLS INLINE off
store_i8_b_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    store_i8_b_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            if (i < current_N && j < current_M) {
                B_mem[out_base + (n0 + i) * M + (m0 + j)] =
                    scheduler_quantize_acc_to_i8(C_buf[i][j], shift);
            }
        }
    }
}

static void store_i8_b_transpose_block(
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M],
    int N,
    int n0,
    int m0,
    int out_base,
    int shift,
    int current_N,
    int current_M
) {
#pragma HLS INLINE off
store_i8_bt_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    store_i8_bt_j:
        for (int j = 0; j < ACCEL_BLOCK_M; j++) {
#pragma HLS PIPELINE II=1
            if (i < current_N && j < current_M) {
                B_mem[out_base + (m0 + j) * N + (n0 + i)] =
                    scheduler_quantize_acc_to_i8(C_buf[i][j], shift);
            }
        }
    }
}

#if GZY_ACCEL_EXPLICIT_BANKS
static void store_c_block_banked(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    gemm_acc_t C_bank[GEMM_TILE][ACCEL_BLOCK_N][ACCEL_BLOCK_M_GROUPS],
    int N,
    int M,
    int n0,
    int m0,
    int c_base,
    int current_N,
    int current_M
) {
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=C_bank complete dim=1
store_c_bank_i:
    for (int i = 0; i < ACCEL_BLOCK_N; i++) {
    store_c_bank_jg:
        for (int jg = 0; jg < ACCEL_BLOCK_M_GROUPS; jg++) {
        store_c_bank_jb:
            for (int jb = 0; jb < GEMM_TILE; jb++) {
#pragma HLS PIPELINE II=1
                const int j = jg * GEMM_TILE + jb;
                if (i < current_N && j < current_M) {
                    C_mem[c_base + (n0 + i) * M + (m0 + j)] = C_bank[jb][i][jg];
                }
            }
        }
    }
}
#endif

void gemm_scheduler(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int N,
    int K,
    int M,
    int a_base,
    int b_base,
    int c_base,
    int load_b_mode,
    int conv_cin,
    int conv_kh,
    int conv_kw,
    int store_mode,
    int store_i8_base,
    int store_i8_shift
) {
#if GZY_ACCEL_SHARED_GEMM_INLINE
#pragma HLS INLINE
#else
#pragma HLS INLINE off
#endif
#if GZY_ACCEL_EXPLICIT_BANKS
    static gemm_data_t A_bank[GEMM_TILE][ACCEL_BLOCK_N][ACCEL_BLOCK_K_GROUPS];
    static gemm_data_t B_bank[GEMM_TILE][ACCEL_BLOCK_K][ACCEL_BLOCK_M_GROUPS];
    static gemm_acc_t C_bank[GEMM_TILE][ACCEL_BLOCK_N][ACCEL_BLOCK_M_GROUPS];
#pragma HLS BIND_STORAGE variable=A_bank type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=B_bank type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=C_bank type=ram_2p impl=bram
#pragma HLS ARRAY_PARTITION variable=A_bank complete dim=1
#pragma HLS ARRAY_PARTITION variable=B_bank complete dim=1
#pragma HLS ARRAY_PARTITION variable=C_bank complete dim=1
#else
    static gemm_data_t A_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_K];
    static gemm_data_t B_buf[ACCEL_BLOCK_K][ACCEL_BLOCK_M];
    static gemm_acc_t C_buf[ACCEL_BLOCK_N][ACCEL_BLOCK_M];
#pragma HLS BIND_STORAGE variable=A_buf type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=B_buf type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=C_buf type=ram_2p impl=bram
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=C_buf cyclic factor=GZY_GEMM_TILE dim=2
#if GZY_ACCEL_LOCAL_ROW_UNROLL > 1
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=GZY_ACCEL_LOCAL_ROW_UNROLL dim=1
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=GZY_ACCEL_LOCAL_ROW_UNROLL dim=1
#pragma HLS ARRAY_PARTITION variable=C_buf cyclic factor=GZY_ACCEL_LOCAL_ROW_UNROLL dim=1
#endif
#endif

block_n_loop:
    for (int n0 = 0; n0 < N; n0 += ACCEL_BLOCK_N) {
    block_m_loop:
        for (int m0 = 0; m0 < M; m0 += ACCEL_BLOCK_M) {
#if GZY_ACCEL_FULL_ONLY
        block_k_loop:
            for (int k0 = 0; k0 < K; k0 += ACCEL_BLOCK_K) {
                const bool reset_c = (k0 == 0);

#if GZY_ACCEL_LOAD_AB_PARALLEL
                load_ab_block_full(
                    A_mem,
                    B_mem,
                    A_buf,
                    B_buf,
                    N,
                    K,
                    M,
                    n0,
                    k0,
                    m0,
                    a_base,
                    b_base
                );
#else
                load_a_block_full(A_mem, A_buf, N, K, n0, k0, a_base);
#if GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD
                if (load_b_mode == GEMM_LOAD_B_CONV_WEIGHT) {
                    load_b_conv_weight_block(
                        B_mem,
                        B_buf,
                        M,
                        k0,
                        m0,
                        b_base,
                        ACCEL_BLOCK_K,
                        ACCEL_BLOCK_M,
                        conv_cin,
                        conv_kh,
                        conv_kw
                    );
                } else {
                    load_b_block_full(B_mem, B_buf, K, M, k0, m0, b_base);
                }
#else
                load_b_block_full(B_mem, B_buf, K, M, k0, m0, b_base);
#endif
#endif
                compute_block_full(A_buf, B_buf, C_buf, reset_c);
            }

            if (store_mode == GEMM_STORE_I8_A) {
                store_i8_a_block(
                    A_mem,
                    C_buf,
                    M,
                    n0,
                    m0,
                    store_i8_base,
                    store_i8_shift,
                    ACCEL_BLOCK_N,
                    ACCEL_BLOCK_M
                );
            } else if (store_mode == GEMM_STORE_I8_B) {
                store_i8_b_block(
                    B_mem,
                    C_buf,
                    M,
                    n0,
                    m0,
                    store_i8_base,
                    store_i8_shift,
                    ACCEL_BLOCK_N,
                    ACCEL_BLOCK_M
                );
            } else if (store_mode == GEMM_STORE_I8_B_TRANSPOSE) {
                store_i8_b_transpose_block(
                    B_mem,
                    C_buf,
                    N,
                    n0,
                    m0,
                    store_i8_base,
                    store_i8_shift,
                    ACCEL_BLOCK_N,
                    ACCEL_BLOCK_M
                );
            } else if (store_mode == GEMM_STORE_ACC_C_TRANSPOSE) {
                store_c_block_transpose(
                    C_mem,
                    C_buf,
                    N,
                    n0,
                    m0,
                    c_base,
                    ACCEL_BLOCK_N,
                    ACCEL_BLOCK_M
                );
            } else {
                store_c_block_full(C_mem, C_buf, N, M, n0, m0, c_base);
            }
#else
            const int current_N = min_int(ACCEL_BLOCK_N, N - n0);
            const int current_M = min_int(ACCEL_BLOCK_M, M - m0);
            const bool full_nm =
                (current_N == ACCEL_BLOCK_N) && (current_M == ACCEL_BLOCK_M);

        block_k_loop:
            for (int k0 = 0; k0 < K; k0 += ACCEL_BLOCK_K) {
                const int current_K = min_int(ACCEL_BLOCK_K, K - k0);
                const bool reset_c = (k0 == 0);
                const bool full_nmk = full_nm && (current_K == ACCEL_BLOCK_K);

#if GZY_ACCEL_EXPLICIT_BANKS
                load_ab_block_banked(
                    A_mem,
                    B_mem,
                    A_bank,
                    B_bank,
                    N,
                    K,
                    M,
                    n0,
                    k0,
                    m0,
                    a_base,
                    b_base,
                    current_N,
                    current_K,
                    current_M
                );
                compute_block_banked(A_bank, B_bank, C_bank, reset_c);
#else
#if GZY_ACCEL_FULL_BLOCK_FAST
                if (full_nmk) {
#if GZY_ACCEL_LOAD_AB_PARALLEL
                    load_ab_block_full(
                        A_mem,
                        B_mem,
                        A_buf,
                        B_buf,
                        N,
                        K,
                        M,
                        n0,
                        k0,
                        m0,
                        a_base,
                        b_base
                    );
#else
                    load_a_block_full(A_mem, A_buf, N, K, n0, k0, a_base);
                    load_b_block_full(B_mem, B_buf, K, M, k0, m0, b_base);
#endif
                    compute_block_full(A_buf, B_buf, C_buf, reset_c);
                } else {
#endif
#if GZY_ACCEL_LOAD_AB_PARALLEL
                    load_ab_block(
                        A_mem,
                        B_mem,
                        A_buf,
                        B_buf,
                        N,
                        K,
                        M,
                        n0,
                        k0,
                        m0,
                        a_base,
                        b_base,
                        current_N,
                        current_K,
                        current_M
                    );
#else
                    load_a_block(A_mem, A_buf, N, K, n0, k0, a_base, current_N, current_K);
#if GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD
                    if (load_b_mode == GEMM_LOAD_B_CONV_WEIGHT) {
                        load_b_conv_weight_block(
                            B_mem,
                            B_buf,
                            M,
                            k0,
                            m0,
                            b_base,
                            current_K,
                            current_M,
                            conv_cin,
                            conv_kh,
                            conv_kw
                        );
                    } else {
                        load_b_block(B_mem, B_buf, K, M, k0, m0, b_base, current_K, current_M);
                    }
#else
                    load_b_block(B_mem, B_buf, K, M, k0, m0, b_base, current_K, current_M);
#endif
#endif
                    compute_block(A_buf, B_buf, C_buf, current_N, current_K, current_M, reset_c);
#if GZY_ACCEL_FULL_BLOCK_FAST
                }
#endif
#endif
            }

#if GZY_ACCEL_EXPLICIT_BANKS
            store_c_block_banked(C_mem, C_bank, N, M, n0, m0, c_base, current_N, current_M);
#else
            if (store_mode == GEMM_STORE_I8_A) {
                store_i8_a_block(
                    A_mem,
                    C_buf,
                    M,
                    n0,
                    m0,
                    store_i8_base,
                    store_i8_shift,
                    current_N,
                    current_M
                );
            } else if (store_mode == GEMM_STORE_I8_B) {
                store_i8_b_block(
                    B_mem,
                    C_buf,
                    M,
                    n0,
                    m0,
                    store_i8_base,
                    store_i8_shift,
                    current_N,
                    current_M
                );
            } else if (store_mode == GEMM_STORE_I8_B_TRANSPOSE) {
                store_i8_b_transpose_block(
                    B_mem,
                    C_buf,
                    N,
                    n0,
                    m0,
                    store_i8_base,
                    store_i8_shift,
                    current_N,
                    current_M
                );
            } else if (store_mode == GEMM_STORE_ACC_C_TRANSPOSE) {
                store_c_block_transpose(
                    C_mem,
                    C_buf,
                    N,
                    n0,
                    m0,
                    c_base,
                    current_N,
                    current_M
                );
            } else {
#if GZY_ACCEL_FULL_BLOCK_FAST
                if (full_nm) {
                    store_c_block_full(C_mem, C_buf, N, M, n0, m0, c_base);
                } else {
                    store_c_block(C_mem, C_buf, N, M, n0, m0, c_base, current_N, current_M);
                }
#else
                store_c_block(C_mem, C_buf, N, M, n0, m0, c_base, current_N, current_M);
#endif
            }
#endif
#endif
        }
    }
}
