#include "accel_tb_common.h"
#include "gemm_scheduler.h"

int main() {
    tb_print_config("V1");
    tb_clear_memories();
    tb_init_gemm_inputs(ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M);

#if GZY_ACCEL_RUNTIME_NKM
    gemm_scheduler_top(g_A_mem, g_B_mem, g_C_mem, ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M);
#else
    gemm_scheduler_top(g_A_mem, g_B_mem, g_C_mem);
#endif
    g_status[0] = 0;

    return tb_check_output("V1", ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M, 0);
}
