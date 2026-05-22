#include "accel_tb_common.h"
#include "accelerator_instruction.h"

int main() {
    tb_print_config("V2");
    tb_clear_memories();
    tb_init_gemm_inputs(ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M);

    g_instr_mem[0] = tb_pack_gemm_instr(ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M, 0, 0, 0);
    g_instr_mem[1] = tb_pack_end_instr();

    instruction_decode_top(g_instr_mem, g_A_mem, g_B_mem, g_C_mem, g_status);

    return tb_check_output("V2", ACCEL_BENCH_N, ACCEL_BENCH_K, ACCEL_BENCH_M, 1);
}
