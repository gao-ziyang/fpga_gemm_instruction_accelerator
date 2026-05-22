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
    word.range(23, 8) = (ap_uint<16>)N;
    word.range(39, 24) = (ap_uint<16>)K;
    word.range(55, 40) = (ap_uint<16>)M;
    word.range(79, 56) = (ap_uint<24>)a_base;
    word.range(103, 80) = (ap_uint<24>)b_base;
    word.range(127, 104) = (ap_uint<24>)c_base;
    return word;
}

static accel_instr_word_t tb_pack_end_instr() {
    accel_instr_word_t word = 0;
    word.range(7, 0) = ACCEL_OP_END;
    return word;
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
