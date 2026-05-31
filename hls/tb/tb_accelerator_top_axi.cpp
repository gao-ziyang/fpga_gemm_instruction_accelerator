#include "accel_tb_common.h"
#include "accelerator_top_axi.h"

int main() {
    tb_print_config("AXI");
    tb_clear_memories();
    tb_init_gemm_inputs(ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M);

    g_instr_mem[0] = tb_pack_gemm_instr(ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M, 0, 0, 0);
    g_instr_mem[1] = tb_pack_end_instr();

    g_status[0] = accelerator_top_axi(g_instr_mem, g_A_mem, g_B_mem, g_C_mem, 2);

    return tb_check_output("AXI", ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M, 1);
}
