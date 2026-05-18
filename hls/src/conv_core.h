#ifndef GZY_CONV_CORE_H
#define GZY_CONV_CORE_H

#include "conv_types.h"

void conv2d_gemm(
    gemm_data_t input[CONV_INPUT_SIZE],
    gemm_data_t weight[CONV_WEIGHT_SIZE],
    gemm_acc_t output[CONV_OUTPUT_SIZE]
);

#endif
