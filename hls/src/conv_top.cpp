#include "conv_top.h"
#include "conv_core.h"

extern "C" {
void conv_top(
    gemm_data_t input[CONV_INPUT_SIZE],
    gemm_data_t weight[CONV_WEIGHT_SIZE],
    gemm_acc_t output[CONV_OUTPUT_SIZE]
) {
#pragma HLS INTERFACE ap_memory port=input
#pragma HLS INTERFACE ap_memory port=weight
#pragma HLS INTERFACE ap_memory port=output
#pragma HLS INTERFACE ap_ctrl_hs port=return

    conv2d_gemm(input, weight, output);
}
}
