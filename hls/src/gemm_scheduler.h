#ifndef GZY_GEMM_SCHEDULER_H
#define GZY_GEMM_SCHEDULER_H

#include "accelerator_types.h"

#ifndef GZY_ACCEL_RUNTIME_NKM
#define GZY_ACCEL_RUNTIME_NKM 0
#endif

void gemm_scheduler(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int N,
    int K,
    int M,
    int a_base,
    int b_base,
    int c_base
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
