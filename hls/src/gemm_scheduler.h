#ifndef GZY_GEMM_SCHEDULER_H
#define GZY_GEMM_SCHEDULER_H

#include "accelerator_types.h"

#ifndef GZY_ACCEL_RUNTIME_NKM
#define GZY_ACCEL_RUNTIME_NKM 0
#endif

static const int GEMM_STORE_ACC_C = 0;
static const int GEMM_STORE_I8_A = 1;
static const int GEMM_STORE_I8_B = 2;
static const int GEMM_STORE_I8_B_TRANSPOSE = 3;
static const int GEMM_STORE_ACC_C_TRANSPOSE = 4;

static const int GEMM_LOAD_B_ROW_MAJOR = 0;
static const int GEMM_LOAD_B_CONV_WEIGHT = 1;

void gemm_scheduler(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int N,
    int K,
    int M,
    int a_base,
    int b_base,
    int c_base,
    int load_b_mode,
    int conv_cin,
    int conv_kh,
    int conv_kw,
    int store_mode,
    int store_i8_base,
    int store_i8_shift
);

extern "C" {
void gemm_scheduler_top(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS]
#if GZY_ACCEL_RUNTIME_NKM
    ,
    int N,
    int K,
    int M
#endif
);
}

#endif
