#include "conv_core.h"
#include "gemm_core.h"

void conv2d_gemm(
    gemm_data_t input[CONV_INPUT_SIZE],
    gemm_data_t weight[CONV_WEIGHT_SIZE],
    gemm_acc_t output[CONV_OUTPUT_SIZE]
) {
#pragma HLS INLINE off

    static gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K];
    static gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M];
    static gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M];
    // A/B/C keep GEMM_MAX_* strides for gemm_tiled(); only the active Conv GEMM
    // region is read or written, so full-matrix clearing would add dead latency.

    int input_window_base = 0;
    int out_w_count = 0;

im2col:
    for (int row = 0; row < CONV_GEMM_N; row++) {
#pragma HLS LOOP_FLATTEN off
        int channel_base = 0;
        int patch_row_base = input_window_base;
        int input_ptr = patch_row_base;
        int kh_count = 0;
        int kw_count = 0;

        // Original layout:
        // A[row][col], row = oh * OUT_W + ow,
        // col = ci * KH * KW + kh * KW + kw.
        // Here row/window and col/input addresses are advanced by counters to
        // avoid division/remainder hardware in the scheduled address path.
        for (int col = 0; col < CONV_GEMM_K; col++) {
#pragma HLS PIPELINE II=1
            A[row][col] = input[input_ptr];

            if (kw_count == CONV_KW - 1) {
                kw_count = 0;
                if (kh_count == CONV_KH - 1) {
                    kh_count = 0;
                    channel_base += CONV_INPUT_C_STRIDE;
                    patch_row_base = channel_base + input_window_base;
                } else {
                    kh_count++;
                    patch_row_base += CONV_INPUT_H_STRIDE;
                }
                input_ptr = patch_row_base;
            } else {
                kw_count++;
                input_ptr++;
            }
        }

        if (out_w_count == CONV_OUT_W - 1) {
            out_w_count = 0;
            input_window_base += CONV_INPUT_H_STRIDE - (CONV_OUT_W - 1) * CONV_STRIDE;
        } else {
            out_w_count++;
            input_window_base += CONV_STRIDE;
        }
    }

flatten_weight:
    int weight_ptr = 0;
    for (int co = 0; co < CONV_COUT; co++) {
#pragma HLS LOOP_FLATTEN off
        // Original layout:
        // B[row][co], row = ci * KH * KW + kh * KW + kw.
        for (int row = 0; row < CONV_GEMM_K; row++) {
#pragma HLS PIPELINE II=1
            B[row][co] = weight[weight_ptr];
            weight_ptr++;
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
    int output_ptr = 0;
    for (int co = 0; co < CONV_COUT; co++) {
#pragma HLS LOOP_FLATTEN off
        for (int row = 0; row < CONV_GEMM_N; row++) {
#pragma HLS PIPELINE II=1
            output[output_ptr] = C[row][co];
            output_ptr++;
        }
    }
}
