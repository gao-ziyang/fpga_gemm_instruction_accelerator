#include "accelerator_instruction.h"

#include "gemm_scheduler.h"

decoded_instr_t decode_instruction(accel_instr_word_t word) {
#pragma HLS INLINE
    decoded_instr_t instr;
    instr.opcode = word.range(7, 0);
    instr.N = (int)word.range(19, 8) + 1;
    instr.K = (int)word.range(31, 20) + 1;
    instr.M = (int)word.range(43, 32) + 1;
    instr.a_base = (int)word.range(49, 44) * ACCEL_BASE_UNIT;
    instr.b_base = (int)word.range(55, 50) * ACCEL_BASE_UNIT;
    instr.c_base = (int)word.range(61, 56) * ACCEL_BASE_UNIT;
    return instr;
}

int execute_instruction_stream(
    accel_instr_word_t instr_mem[ACCEL_MAX_INSTR],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int instr_num
) {
#pragma HLS INLINE off
    int executed = 0;
    bool done = false;

instr_loop:
    for (int pc = 0; pc < ACCEL_MAX_INSTR; pc++) {
        if (pc < instr_num && !done) {
            decoded_instr_t instr = decode_instruction(instr_mem[pc]);

            if (instr.opcode == ACCEL_OP_END) {
                done = true;
            } else if (instr.opcode == ACCEL_OP_GEMM) {
                gemm_scheduler(
                    A_mem,
                    B_mem,
                    C_mem,
                    instr.N,
                    instr.K,
                    instr.M,
                    instr.a_base,
                    instr.b_base,
                    instr.c_base
                );
                executed++;
            } else {
                return -1;
            }
        }
    }

    return executed;
}
