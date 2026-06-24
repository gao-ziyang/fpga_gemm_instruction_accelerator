#ifndef GZY_ACCEL_TB_COMMON_H
#define GZY_ACCEL_TB_COMMON_H

#include <cstdio>

#include "accelerator_types.h"

static gemm_data_t g_A_mem[ACCEL_A_ELEMS];
static gemm_data_t g_B_mem[ACCEL_B_ELEMS];
static gemm_acc_t g_C_mem[ACCEL_C_ELEMS];
static accel_instr_word_t g_instr_mem[ACCEL_MAX_INSTR];
static int g_status[1];

static int tb_i_term(int i) {
    return (i % 7) - 3;
}

static int tb_k_a_term(int k) {
    return (k % 5) - 2;
}

static int tb_j_term(int j) {
    return (j % 11) - 5;
}

static int tb_k_b_term(int k) {
    return (k % 3) - 1;
}

static gemm_data_t tb_gen_a(int i, int k) {
    return (gemm_data_t)(tb_i_term(i) + tb_k_a_term(k));
}

static gemm_data_t tb_gen_b(int k, int j) {
    return (gemm_data_t)(tb_j_term(j) + tb_k_b_term(k));
}

static void tb_compute_k_sums(
    int K,
    long long &sum_f,
    long long &sum_g,
    long long &sum_fg
) {
    sum_f = 0;
    sum_g = 0;
    sum_fg = 0;
    for (int k = 0; k < K; k++) {
        const int f = tb_k_a_term(k);
        const int g = tb_k_b_term(k);
        sum_f += f;
        sum_g += g;
        sum_fg += f * g;
    }
}

static gemm_acc_t tb_golden_value(
    int i,
    int j,
    int K,
    long long sum_f,
    long long sum_g,
    long long sum_fg
) {
    const long long ai = tb_i_term(i);
    const long long bj = tb_j_term(j);
    const long long value =
        (long long)K * ai * bj +
        ai * sum_g +
        bj * sum_f +
        sum_fg;
    return (gemm_acc_t)value;
}

static void tb_clear_memories() {
    for (int i = 0; i < ACCEL_A_ELEMS; i++) {
        g_A_mem[i] = 0;
    }
    for (int i = 0; i < ACCEL_B_ELEMS; i++) {
        g_B_mem[i] = 0;
    }
    for (int i = 0; i < ACCEL_C_ELEMS; i++) {
        g_C_mem[i] = 0;
    }
    for (int i = 0; i < ACCEL_MAX_INSTR; i++) {
        g_instr_mem[i] = 0;
    }
    g_status[0] = -99;
}

static void tb_init_gemm_inputs(int N, int K, int M) {
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < K; k++) {
            g_A_mem[i * K + k] = tb_gen_a(i, k);
        }
    }

    for (int k = 0; k < K; k++) {
        for (int j = 0; j < M; j++) {
            g_B_mem[k * M + j] = tb_gen_b(k, j);
        }
    }
}

static accel_instr_word_t tb_pack_gemm_instr(
    int N,
    int K,
    int M,
    int a_base,
    int b_base,
    int c_base
) {
    accel_instr_word_t word = 0;
    word.range(7, 0) = ACCEL_OP_GEMM;
    word.range(19, 8) = (ap_uint<12>)(N - 1);
    word.range(31, 20) = (ap_uint<12>)(K - 1);
    word.range(43, 32) = (ap_uint<12>)(M - 1);
    word.range(49, 44) = (ap_uint<6>)(a_base / ACCEL_BASE_UNIT);
    word.range(55, 50) = (ap_uint<6>)(b_base / ACCEL_BASE_UNIT);
    word.range(61, 56) = (ap_uint<6>)(c_base / ACCEL_BASE_UNIT);
    return word;
}

static accel_instr_word_t tb_pack_layer_instr(
    ap_uint<8> opcode,
    int N,
    int K,
    int M,
    int a_base,
    int b_base,
    int c_base
) {
    accel_instr_word_t word = 0;
    word.range(7, 0) = opcode;
    word.range(19, 8) = (ap_uint<12>)(N - 1);
    word.range(31, 20) = (ap_uint<12>)(K - 1);
    word.range(43, 32) = (ap_uint<12>)(M - 1);
    word.range(49, 44) = (ap_uint<6>)(a_base / ACCEL_BASE_UNIT);
    word.range(55, 50) = (ap_uint<6>)(b_base / ACCEL_BASE_UNIT);
    word.range(61, 56) = (ap_uint<6>)(c_base / ACCEL_BASE_UNIT);
    return word;
}

static accel_instr_word_t tb_pack_end_instr() {
    accel_instr_word_t word = 0;
    word.range(7, 0) = ACCEL_OP_END;
    return word;
}

static gemm_data_t tb_gen_a_seed(int i, int k, int seed) {
    return (gemm_data_t)(((i * 17 + k * 31 + seed * 7 + 13) % 64) - 32);
}

static gemm_data_t tb_gen_b_seed(int k, int j, int seed) {
    return (gemm_data_t)(((k * 29 + j * 11 + seed * 5 + 3) % 64) - 32);
}

static void tb_init_a_region(int N, int K, int a_base, int seed) {
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < K; k++) {
            g_A_mem[a_base + i * K + k] = tb_gen_a_seed(i, k, seed);
        }
    }
}

static void tb_init_b_region(int K, int M, int b_base, int seed) {
    for (int k = 0; k < K; k++) {
        for (int j = 0; j < M; j++) {
            g_B_mem[b_base + k * M + j] = tb_gen_b_seed(k, j, seed);
        }
    }
}

static gemm_acc_t tb_golden_from_mem(
    int i,
    int j,
    int K,
    int M,
    int a_base,
    int b_base
) {
    gemm_acc_t sum = 0;
    for (int k = 0; k < K; k++) {
        sum += (gemm_acc_t)g_A_mem[a_base + i * K + k] *
               (gemm_acc_t)g_B_mem[b_base + k * M + j];
    }
    return sum;
}

static int tb_check_output_from_mem(
    const char *tag,
    int N,
    int K,
    int M,
    int a_base,
    int b_base,
    int c_base,
    int expected_status
) {
    int errors = 0;
    int max_abs_error = 0;
    long long checksum = 0;

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < M; j++) {
            const int got = (int)g_C_mem[c_base + i * M + j];
            const int ref = (int)tb_golden_from_mem(i, j, K, M, a_base, b_base);
            const int diff = got - ref;
            const int abs_diff = diff < 0 ? -diff : diff;
            if (abs_diff > max_abs_error) {
                max_abs_error = abs_diff;
            }
            checksum += (long long)got * (long long)((i * M + j) % 4099 + 1);
            if (got != ref) {
                if (errors < 16) {
                    std::printf("[%s][ERR] C[%d][%d] got %d, expected %d\n", tag, i, j, got, ref);
                }
                errors++;
            }
        }
    }

    std::printf("[%s] status=%d expected_status=%d\n", tag, g_status[0], expected_status);
    std::printf("[%s] mismatch_count=%d\n", tag, errors);
    std::printf("[%s] max_abs_error=%d\n", tag, max_abs_error);
    std::printf("[%s] checksum=%lld\n", tag, checksum);

    if (expected_status != -999 && g_status[0] != expected_status) {
        std::printf("[%s] FAIL: unexpected status\n", tag);
        return 1;
    }

    if (errors == 0) {
        std::printf("[%s] PASS\n", tag);
        return 0;
    }

    std::printf("[%s] FAIL, errors=%d\n", tag, errors);
    return 1;
}

static int tb_check_output(const char *tag, int N, int K, int M, int expected_status) {
    long long sum_f = 0;
    long long sum_g = 0;
    long long sum_fg = 0;
    tb_compute_k_sums(K, sum_f, sum_g, sum_fg);

    int errors = 0;
    int max_abs_error = 0;
    long long checksum = 0;

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < M; j++) {
            const int got = (int)g_C_mem[i * M + j];
            const int ref = (int)tb_golden_value(i, j, K, sum_f, sum_g, sum_fg);
            const int diff = got - ref;
            const int abs_diff = diff < 0 ? -diff : diff;
            if (abs_diff > max_abs_error) {
                max_abs_error = abs_diff;
            }
            checksum += (long long)got * (long long)((i * M + j) % 4099 + 1);
            if (got != ref) {
                if (errors < 16) {
                    std::printf("[%s][ERR] C[%d][%d] got %d, expected %d\n", tag, i, j, got, ref);
                }
                errors++;
            }
        }
    }

    std::printf("[%s] status=%d expected_status=%d\n", tag, g_status[0], expected_status);
    std::printf("[%s] mismatch_count=%d\n", tag, errors);
    std::printf("[%s] max_abs_error=%d\n", tag, max_abs_error);
    std::printf("[%s] checksum=%lld\n", tag, checksum);

    if (expected_status != -999 && g_status[0] != expected_status) {
        std::printf("[%s] FAIL: unexpected status\n", tag);
        return 1;
    }

    if (errors == 0) {
        std::printf("[%s] PASS\n", tag);
        return 0;
    }

    std::printf("[%s] FAIL, errors=%d\n", tag, errors);
    return 1;
}

static const int TB_LAYER_N = 16;
static const int TB_LAYER_D = 96;
static const int TB_LAYER_SCORE_M = 16;
static const int TB_LAYER_CONV_K = 27;
static const int TB_LAYER_CONV_M = 4;

static const int TB_A_QKV_BASE = 0;
static const int TB_A_CONV_BASE = ACCEL_BASE_UNIT;
static const int TB_B_Q_BASE = 0;
static const int TB_B_K_BASE = ACCEL_QKV_B_STRIDE;
static const int TB_B_V_BASE = 2 * ACCEL_QKV_B_STRIDE;
static const int TB_B_SCORE_BASE = 3 * ACCEL_QKV_B_STRIDE;
static const int TB_B_CONV_BASE = 4 * ACCEL_QKV_B_STRIDE;
static const int TB_C_Q_BASE = 0;
static const int TB_C_K_BASE = ACCEL_QKV_C_STRIDE;
static const int TB_C_V_BASE = 2 * ACCEL_QKV_C_STRIDE;
static const int TB_C_SCORE_BASE = 3 * ACCEL_QKV_C_STRIDE;
static const int TB_C_CONV_BASE = 4 * ACCEL_QKV_C_STRIDE;

static void tb_init_layer_instruction_case() {
    tb_clear_memories();

    tb_init_a_region(TB_LAYER_N, TB_LAYER_D, TB_A_QKV_BASE, 1);
    tb_init_a_region(TB_LAYER_N, TB_LAYER_CONV_K, TB_A_CONV_BASE, 5);

    tb_init_b_region(TB_LAYER_D, TB_LAYER_D, TB_B_Q_BASE, 2);
    tb_init_b_region(TB_LAYER_D, TB_LAYER_D, TB_B_K_BASE, 3);
    tb_init_b_region(TB_LAYER_D, TB_LAYER_D, TB_B_V_BASE, 4);
    tb_init_b_region(TB_LAYER_D, TB_LAYER_SCORE_M, TB_B_SCORE_BASE, 6);
    tb_init_b_region(TB_LAYER_CONV_K, TB_LAYER_CONV_M, TB_B_CONV_BASE, 7);

    g_instr_mem[0] = tb_pack_layer_instr(
        ACCEL_OP_QKV,
        TB_LAYER_N,
        TB_LAYER_D,
        TB_LAYER_D,
        TB_A_QKV_BASE,
        TB_B_Q_BASE,
        TB_C_Q_BASE
    );
    g_instr_mem[1] = tb_pack_layer_instr(
        ACCEL_OP_ATTN_SCORE,
        TB_LAYER_N,
        TB_LAYER_D,
        TB_LAYER_SCORE_M,
        TB_A_QKV_BASE,
        TB_B_SCORE_BASE,
        TB_C_SCORE_BASE
    );
    g_instr_mem[2] = tb_pack_layer_instr(
        ACCEL_OP_CONV_GEMM,
        TB_LAYER_N,
        TB_LAYER_CONV_K,
        TB_LAYER_CONV_M,
        TB_A_CONV_BASE,
        TB_B_CONV_BASE,
        TB_C_CONV_BASE
    );
    g_instr_mem[3] = tb_pack_end_instr();
}

static int tb_check_layer_instruction_case(const char *tag, int expected_status) {
    int errors = 0;

    errors += tb_check_output_from_mem(
        "QKV_Q",
        TB_LAYER_N,
        TB_LAYER_D,
        TB_LAYER_D,
        TB_A_QKV_BASE,
        TB_B_Q_BASE,
        TB_C_Q_BASE,
        expected_status
    );
    errors += tb_check_output_from_mem(
        "QKV_K",
        TB_LAYER_N,
        TB_LAYER_D,
        TB_LAYER_D,
        TB_A_QKV_BASE,
        TB_B_K_BASE,
        TB_C_K_BASE,
        expected_status
    );
    errors += tb_check_output_from_mem(
        "QKV_V",
        TB_LAYER_N,
        TB_LAYER_D,
        TB_LAYER_D,
        TB_A_QKV_BASE,
        TB_B_V_BASE,
        TB_C_V_BASE,
        expected_status
    );
    errors += tb_check_output_from_mem(
        "ATTN_SCORE",
        TB_LAYER_N,
        TB_LAYER_D,
        TB_LAYER_SCORE_M,
        TB_A_QKV_BASE,
        TB_B_SCORE_BASE,
        TB_C_SCORE_BASE,
        expected_status
    );
    errors += tb_check_output_from_mem(
        "CONV_GEMM",
        TB_LAYER_N,
        TB_LAYER_CONV_K,
        TB_LAYER_CONV_M,
        TB_A_CONV_BASE,
        TB_B_CONV_BASE,
        TB_C_CONV_BASE,
        expected_status
    );

    if (errors == 0) {
        std::printf("[%s] PASS layer instruction stream\n", tag);
        return 0;
    }

    std::printf("[%s] FAIL layer instruction stream, failed_sections=%d\n", tag, errors);
    return 1;
}

static void tb_print_config(const char *tag) {
    const long long total_mac =
        (long long)ACCEL_BENCH_N * (long long)ACCEL_BENCH_K * (long long)ACCEL_BENCH_M;

    std::printf(
        "[%s] N=%d K=%d M=%d TILE=%d BLOCK_N=%d BLOCK_K=%d BLOCK_M=%d total_mac=%lld\n",
        tag,
        ACCEL_BENCH_N,
        ACCEL_BENCH_K,
        ACCEL_BENCH_M,
        GEMM_TILE,
        ACCEL_BLOCK_N,
        ACCEL_BLOCK_K,
        ACCEL_BLOCK_M,
        total_mac
    );
}

#endif
