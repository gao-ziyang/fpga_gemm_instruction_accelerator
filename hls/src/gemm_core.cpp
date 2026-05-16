#include "gemm_core.h"

void gemm_tiled(
    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int K,
    int M,
    bool update_A
) {
#pragma HLS INLINE off

    static gemm_data_t A_bram[GEMM_MAX_N][GEMM_MAX_K];
    static gemm_data_t B_bram[GEMM_MAX_K][GEMM_BLOCK_M];
#pragma HLS BIND_STORAGE variable=A_bram type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=B_bram type=ram_2p impl=bram

    if (update_A) {
load_a_bram:
        for (int i = 0; i < N; i++) {
            for (int k = 0; k < K; k++) {
#pragma HLS PIPELINE II=1
                A_bram[i][k] = A[i][k];
            }
        }
    }

block_m_loop:
    for (int j_block = 0; j_block < M; j_block += GEMM_BLOCK_M) {
        const int current_block_M =
            (j_block + GEMM_BLOCK_M <= M) ? GEMM_BLOCK_M : (M - j_block);

    load_b_bram:
        for (int k = 0; k < K; k++) {
            for (int j = 0; j < current_block_M; j++) {
#pragma HLS PIPELINE II=1
                B_bram[k][j] = B[k][j_block + j];
            }
        }

tile_i_loop:
        for (int i0 = 0; i0 < N; i0 += GEMM_TILE) {
    tile_j_loop:
            for (int j0 = 0; j0 < current_block_M; j0 += GEMM_TILE) {
                gemm_data_t localA[GEMM_TILE][GEMM_TILE];
                gemm_data_t localB[GEMM_TILE][GEMM_TILE];
                gemm_acc_t localC[GEMM_TILE][GEMM_TILE];
#pragma HLS ARRAY_PARTITION variable=localC complete dim=0
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0

            init_c:
                for (int ii = 0; ii < GEMM_TILE; ii++) {
#pragma HLS UNROLL
                    for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                        localC[ii][jj] = 0;
                    }
                }

            tile_k_loop:
                for (int k0 = 0; k0 < K; k0 += GEMM_TILE) {
                load_local_a:
                    for (int ii = 0; ii < GEMM_TILE; ii++) {
                        for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS PIPELINE II=1
                            int i_global = i0 + ii;
                            int k_global = k0 + kk;
                            if (i_global < N && k_global < K) {
                                localA[ii][kk] = A_bram[i_global][k_global];
                            } else {
                                localA[ii][kk] = 0;
                            }
                        }
                    }

                load_local_b:
                    for (int kk = 0; kk < GEMM_TILE; kk++) {
                        for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS PIPELINE II=1
                            int k_global = k0 + kk;
                            int j_local = j0 + jj;
                            if (k_global < K && j_local < current_block_M) {
                                localB[kk][jj] = B_bram[k_global][j_local];
                            } else {
                                localB[kk][jj] = 0;
                            }
                        }
                    }

                dot_k:
                    for (int kk = 0; kk < GEMM_TILE; kk++) {
#pragma HLS PIPELINE II=1
                        for (int ii = 0; ii < GEMM_TILE; ii++) {
#pragma HLS UNROLL
                            gemm_acc_t a_val = (gemm_acc_t)localA[ii][kk];
                            for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS UNROLL
                                gemm_acc_t b_val = (gemm_acc_t)localB[kk][jj];
                                localC[ii][jj] += a_val * b_val;
                            }
                        }
                    }
                }

            write_c:
                for (int ii = 0; ii < GEMM_TILE; ii++) {
                    for (int jj = 0; jj < GEMM_TILE; jj++) {
#pragma HLS PIPELINE II=1
                        int i_global = i0 + ii;
                        int j_global = j_block + j0 + jj;
                        if (i_global < N && j_global < M) {
                            C[i_global][j_global] = localC[ii][jj] >> GEMM_OUT_SHIFT;
                        }
                    }
                }
            }
        }
    }
}
