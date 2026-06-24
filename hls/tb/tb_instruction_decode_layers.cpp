#include "accel_tb_common.h"
#include "accelerator_instruction.h"

int main() {
    tb_print_config("DECODE_LAYERS");
    tb_init_layer_instruction_case();

    instruction_decode_top(g_instr_mem, g_A_mem, g_B_mem, g_C_mem, g_status);

    return tb_check_layer_instruction_case("DECODE_LAYERS", 3);
}
