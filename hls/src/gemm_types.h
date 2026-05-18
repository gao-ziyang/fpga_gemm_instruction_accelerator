#ifndef GZY_GEMM_TYPES_H
#define GZY_GEMM_TYPES_H

#include <ap_int.h>

static const int GEMM_MAX_N = 16;
static const int GEMM_MAX_K = 96;
static const int GEMM_MAX_M = 96;
// 4x4 local tile; this keeps the MAC array small while larger matrices use more loop iterations.
static const int GEMM_TILE = 4;
static const int GEMM_BLOCK_M = 8;

typedef ap_int<8> gemm_data_t;
typedef ap_int<32> gemm_acc_t;

#endif
