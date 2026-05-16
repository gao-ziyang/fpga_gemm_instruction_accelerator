#ifndef GZY_GEMM_TYPES_H
#define GZY_GEMM_TYPES_H

#include <ap_int.h>

static const int GEMM_DIM = 4;

typedef ap_int<8> gemm_data_t;
typedef ap_int<32> gemm_acc_t;

#endif
