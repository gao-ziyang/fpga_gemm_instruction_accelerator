#include "conv_top.h"
#include "conv_core.h"

extern "C" {
void conv_top(
    gemm_data_t input[CONV_CIN][CONV_IN_H][CONV_IN_W],
    gemm_data_t weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW],
    gemm_acc_t output[CONV_COUT][CONV_OUT_H][CONV_OUT_W]
) {
#pragma HLS INTERFACE ap_memory port=input
#pragma HLS INTERFACE ap_memory port=weight
#pragma HLS INTERFACE ap_memory port=output
#pragma HLS INTERFACE ap_ctrl_hs port=return

    conv2d_gemm(input, weight, output);
}
}
