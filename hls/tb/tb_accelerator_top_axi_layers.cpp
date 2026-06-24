#include "accel_tb_common.h"
#include "accelerator_top_axi.h"

int main() {
    tb_print_config("AXI_LAYERS");
    tb_init_layer_instruction_case();

    g_status[0] = accelerator_top_axi(g_instr_mem, g_A_mem, g_B_mem, g_C_mem, 4);

    return tb_check_layer_instruction_case("AXI_LAYERS", 3);
}
