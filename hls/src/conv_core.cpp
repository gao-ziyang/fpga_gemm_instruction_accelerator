#include "conv_core.h"
#include "gemm_core.h"

void conv2d_gemm(
    gemm_data_t input[CONV_CIN][CONV_IN_H][CONV_IN_W],
    gemm_data_t weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW],
    gemm_acc_t output[CONV_COUT][CONV_OUT_H][CONV_OUT_W]
) {
#pragma HLS INLINE off

    static gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K];
    static gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M];
    static gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M];
    // A/B/C keep GEMM_MAX_* strides for gemm_tiled(); only the active Conv GEMM
    // region is read or written, so full-matrix clearing would add dead latency.

im2col:
    for (int oh = 0; oh < CONV_OUT_H; oh++) {
#pragma HLS LOOP_FLATTEN off
        for (int ow = 0; ow < CONV_OUT_W; ow++) {
            const int row = oh * CONV_OUT_W + ow;
            int col = 0;
            for (int ci = 0; ci < CONV_CIN; ci++) {
                for (int kh = 0; kh < CONV_KH; kh++) {
                    for (int kw = 0; kw < CONV_KW; kw++) {
#pragma HLS PIPELINE II=1
                        const int ih = oh * CONV_STRIDE + kh;
                        const int iw = ow * CONV_STRIDE + kw;
                        A[row][col] = input[ci][ih][iw];
                        col++;
                    }
                }
            }
        }
    }

flatten_weight:
    for (int co = 0; co < CONV_COUT; co++) {
#pragma HLS LOOP_FLATTEN off
        int row = 0;
        for (int ci = 0; ci < CONV_CIN; ci++) {
            for (int kh = 0; kh < CONV_KH; kh++) {
                for (int kw = 0; kw < CONV_KW; kw++) {
#pragma HLS PIPELINE II=1
                    B[row][co] = weight[co][ci][kh][kw];
                    row++;
                }
            }
        }
    }

    gemm_tiled(
        A,
        B,
        C,
        CONV_GEMM_N,
        CONV_GEMM_K,
        CONV_GEMM_M,
        true
    );

reshape_output:
    for (int oh = 0; oh < CONV_OUT_H; oh++) {
#pragma HLS LOOP_FLATTEN off
        for (int ow = 0; ow < CONV_OUT_W; ow++) {
            const int row = oh * CONV_OUT_W + ow;
            for (int co = 0; co < CONV_COUT; co++) {
#pragma HLS PIPELINE II=1
                output[co][oh][ow] = C[row][co];
            }
        }
    }
}
