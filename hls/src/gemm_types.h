#ifndef GZY_GEMM_TYPES_H
#define GZY_GEMM_TYPES_H

#include <ap_int.h>

static const int GEMM_MAX_N = 8;
static const int GEMM_MAX_K = 8;
static const int GEMM_MAX_M = 8;
static const int GEMM_TILE = 4;//4x4的tile，局部计算矩阵的大小，越大并行度越高，但资源消耗也越大，综合工具会自动调整到合适的值
static const int GEMM_BLOCK_M = 8;
static const int GEMM_OUT_SHIFT = 8;//右移动缩放8位

typedef ap_int<8> gemm_data_t;
typedef ap_int<32> gemm_acc_t;

#endif
