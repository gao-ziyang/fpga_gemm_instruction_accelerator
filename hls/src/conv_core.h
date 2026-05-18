#ifndef GZY_CONV_CORE_H
#define GZY_CONV_CORE_H

#include "conv_types.h"

void conv2d_gemm(
    gemm_data_t input[CONV_CIN][CONV_IN_H][CONV_IN_W],
    gemm_data_t weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW],
    gemm_acc_t output[CONV_COUT][CONV_OUT_H][CONV_OUT_W]
);

#endif
