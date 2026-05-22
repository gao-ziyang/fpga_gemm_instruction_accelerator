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

static const int ACCEL_LOCAL_ROW_UNROLL = GZY_ACCEL_LOCAL_ROW_UNROLL;
static const int ACCEL_LOAD_AB_COLS =
    (ACCEL_BLOCK_N > ACCEL_BLOCK_M) ? ACCEL_BLOCK_N : ACCEL_BLOCK_M;

static int min_int(int a, int b) {
#pragma HLS INLINE
    return (a < b) ? a : b;
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
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=GZY_GEMM_TILE dim=2
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=GZY_GEMM_TILE dim=2
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
                        if (ii >= GEMM_TILE || reset_c) {
                            if (ii < GEMM_TILE) {
                                localC[ii][jj] = 0;
                            }
                        } else if (bi < current_N && bj < current_M) {
                            localC[ii][jj] = C_buf[bi][bj];
                        } else {
                            localC[ii][jj] = 0;
                        }
                    }
                }
            }

        compute_tile_k:
            for (int tk = 0; tk < ACCEL_BLOCK_K; tk += GEMM_TILE) {
                gemm_data_t localA[GEMM_TILE][GEMM_TILE];
                gemm_data_t localB[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0

#if GZY_ACCEL_LOCAL_AB_PARALLEL
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
                            if (ii < GEMM_TILE && bi < current_N && bk < current_K) {
                                localA[ii][kk] = A_buf[bi][bk];
                            } else if (ii < GEMM_TILE) {
                                localA[ii][kk] = 0;
                            }
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
                            if (kk < GEMM_TILE && bk < current_K && bj < current_M) {
                                localB[kk][jj] = B_buf[bk][bj];
                            } else if (kk < GEMM_TILE) {
                                localB[kk][jj] = 0;
                            }
                        }
                    }
                }
#endif

                gemm_core_mac(localA, localB, localC);
            }

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
                        if (ii < GEMM_TILE && bi < current_N && bj < current_M) {
                            C_buf[bi][bj] = localC[ii][jj];
                        }
                    }
                }
            }
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

void gemm_scheduler(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int N,
    int K,
    int M,
    int a_base,
    int b_base,
    int c_base
) {
#pragma HLS INLINE off
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

block_n_loop:
    for (int n0 = 0; n0 < N; n0 += ACCEL_BLOCK_N) {
    block_m_loop:
        for (int m0 = 0; m0 < M; m0 += ACCEL_BLOCK_M) {
            const int current_N = min_int(ACCEL_BLOCK_N, N - n0);
            const int current_M = min_int(ACCEL_BLOCK_M, M - m0);

        block_k_loop:
            for (int k0 = 0; k0 < K; k0 += ACCEL_BLOCK_K) {
                const int current_K = min_int(ACCEL_BLOCK_K, K - k0);
                const bool reset_c = (k0 == 0);

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
                load_b_block(B_mem, B_buf, K, M, k0, m0, b_base, current_K, current_M);
#endif
                compute_block(A_buf, B_buf, C_buf, current_N, current_K, current_M, reset_c);
            }

            store_c_block(C_mem, C_buf, N, M, n0, m0, c_base, current_N, current_M);
        }
    }
}
