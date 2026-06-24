#include <cstdio>

#include "accel_tb_common.h"
#include "accelerator_top_axi.h"

static const int TB_OP_N = 16;
static const int TB_OP_D = 96;
static const int TB_OP_Q_SHIFT = 5;
static const int TB_OP_P_SHIFT = 6;

static const int TB_CONV_CIN = 3;
static const int TB_CONV_IN_H = 6;
static const int TB_CONV_IN_W = 6;
static const int TB_CONV_COUT = 4;
static const int TB_CONV_KH = 3;
static const int TB_CONV_KW = 3;
static const int TB_CONV_STRIDE = 1;
static const int TB_CONV_OUT_H =
    (TB_CONV_IN_H - TB_CONV_KH) / TB_CONV_STRIDE + 1;
static const int TB_CONV_OUT_W =
    (TB_CONV_IN_W - TB_CONV_KW) / TB_CONV_STRIDE + 1;
static const int TB_CONV_OUT_HW = TB_CONV_OUT_H * TB_CONV_OUT_W;

static const int TB_A_X = 0;
static const int TB_A_Q = 2048;
static const int TB_A_P = 4096;
static const int TB_A_CONV_IN = 4608;
static const int TB_A_CONV_IM2COL = 5120;

static const int TB_B_WQ = 0;
static const int TB_B_WK = 12288;
static const int TB_B_WV = 24576;
static const int TB_B_KT = 36864;
static const int TB_B_V = 40960;
static const int TB_B_CONV_W = 45056;
static const int TB_B_CONV_W_SCRATCH = 49152;
static const int TB_B_CONV_W_PREPACKED = TB_B_CONV_W_SCRATCH;

static const int TB_C_QKV_SCRATCH = 0;
static const int TB_C_SCORE = 4096;
static const int TB_C_OUT = 8192;
static const int TB_C_CONV_OUT = 12288;
static const int TB_C_CONV_SCRATCH = 16384;

#ifndef GZY_ACCEL_TB_ATTN_NORM_POW2_APPROX
#define GZY_ACCEL_TB_ATTN_NORM_POW2_APPROX 0
#endif

static int tb_abs_int(int x) {
    return x < 0 ? -x : x;
}

static gemm_data_t tb_pattern_i8(int index, int seed, int span) {
    return (gemm_data_t)(((index * 17 + seed * 23 + 11) % span) - (span / 2));
}

static gemm_data_t tb_quantize_i8(gemm_acc_t value, int shift) {
    gemm_acc_t shifted = value;
    if (shift > 0) {
        shifted = value >> shift;
    }

    if (shifted > 127) {
        return (gemm_data_t)127;
    }
    if (shifted < -128) {
        return (gemm_data_t)-128;
    }
    return (gemm_data_t)shifted;
}

static int tb_ceil_log2_positive(gemm_acc_t value) {
    unsigned int v = (unsigned int)value;
    unsigned int tmp = v;
    int shift = 0;

    if (tmp > 0xFFFFu) {
        tmp >>= 16;
        shift += 16;
    }
    if (tmp > 0xFFu) {
        tmp >>= 8;
        shift += 8;
    }
    if (tmp > 0xFu) {
        tmp >>= 4;
        shift += 4;
    }
    if (tmp > 0x3u) {
        tmp >>= 2;
        shift += 2;
    }
    if (tmp > 0x1u) {
        shift += 1;
    }

    if ((v & (v - 1u)) != 0u) {
        shift += 1;
    }
    return shift;
}

static accel_instr_word_t tb_pack_descriptor_header(
    ap_uint<8> opcode,
    int len,
    int arg0,
    int arg1,
    int arg2,
    int arg3
) {
    accel_instr_word_t word = 0;
    word.range(7, 0) = opcode;
    word.range(15, 8) = (ap_uint<8>)len;
    word.range(27, 16) = (ap_uint<12>)arg0;
    word.range(39, 28) = (ap_uint<12>)arg1;
    word.range(51, 40) = (ap_uint<12>)arg2;
    word.range(63, 52) = (ap_uint<12>)arg3;
    return word;
}

static accel_instr_word_t tb_pack_offsets(int lo, int hi) {
    accel_instr_word_t word = 0;
    word.range(31, 0) = (ap_uint<32>)lo;
    word.range(63, 32) = (ap_uint<32>)hi;
    return word;
}

static accel_instr_word_t tb_pack_u16x4(int a, int b, int c, int d) {
    accel_instr_word_t word = 0;
    word.range(15, 0) = (ap_uint<16>)a;
    word.range(31, 16) = (ap_uint<16>)b;
    word.range(47, 32) = (ap_uint<16>)c;
    word.range(63, 48) = (ap_uint<16>)d;
    return word;
}

static gemm_acc_t tb_qkv_sum(int row, int col, int weight_base) {
    gemm_acc_t sum = 0;
    for (int d = 0; d < TB_OP_D; d++) {
        sum += (gemm_acc_t)g_A_mem[TB_A_X + row * TB_OP_D + d] *
               (gemm_acc_t)g_B_mem[weight_base + d * TB_OP_D + col];
    }
    return sum;
}

static void tb_init_operator_descriptor_case(int &instr_words) {
    tb_clear_memories();

    for (int i = 0; i < TB_OP_N; i++) {
        for (int d = 0; d < TB_OP_D; d++) {
            g_A_mem[TB_A_X + i * TB_OP_D + d] =
                tb_pattern_i8(i * TB_OP_D + d, 1, 9);
        }
    }

    for (int d = 0; d < TB_OP_D; d++) {
        for (int j = 0; j < TB_OP_D; j++) {
            g_B_mem[TB_B_WQ + d * TB_OP_D + j] =
                tb_pattern_i8(d * TB_OP_D + j, 2, 7);
            g_B_mem[TB_B_WK + d * TB_OP_D + j] =
                tb_pattern_i8(d * TB_OP_D + j, 3, 7);
            g_B_mem[TB_B_WV + d * TB_OP_D + j] =
                tb_pattern_i8(d * TB_OP_D + j, 4, 7);
        }
    }

    for (int c = 0; c < TB_CONV_CIN; c++) {
        for (int h = 0; h < TB_CONV_IN_H; h++) {
            for (int w = 0; w < TB_CONV_IN_W; w++) {
                const int idx = c * TB_CONV_IN_H * TB_CONV_IN_W +
                                h * TB_CONV_IN_W + w;
                g_A_mem[TB_A_CONV_IN + idx] = tb_pattern_i8(idx, 5, 9);
            }
        }
    }

    for (int co = 0; co < TB_CONV_COUT; co++) {
        for (int ci = 0; ci < TB_CONV_CIN; ci++) {
            for (int r = 0; r < TB_CONV_KH; r++) {
                for (int s = 0; s < TB_CONV_KW; s++) {
                    const int idx = ((co * TB_CONV_CIN + ci) * TB_CONV_KH + r) *
                                    TB_CONV_KW + s;
                    g_B_mem[TB_B_CONV_W + idx] = tb_pattern_i8(idx, 6, 7);
                }
            }
        }
    }

#if GZY_ACCEL_TB_CONV_PREPACKED_WEIGHT
    for (int ci = 0; ci < TB_CONV_CIN; ci++) {
        for (int r = 0; r < TB_CONV_KH; r++) {
            for (int s = 0; s < TB_CONV_KW; s++) {
                const int col = (ci * TB_CONV_KH + r) * TB_CONV_KW + s;
                for (int co = 0; co < TB_CONV_COUT; co++) {
                    const int src_idx = TB_B_CONV_W +
                        ((co * TB_CONV_CIN + ci) * TB_CONV_KH + r) * TB_CONV_KW + s;
                    g_B_mem[TB_B_CONV_W_PREPACKED + col * TB_CONV_COUT + co] =
                        g_B_mem[src_idx];
                }
            }
        }
    }
#endif

    int pc = 0;
    g_instr_mem[pc++] = tb_pack_descriptor_header(ACCEL_OP_CONV2D, 6, 0, 0, 0, 0);
    g_instr_mem[pc++] = tb_pack_u16x4(
        TB_CONV_CIN,
        TB_CONV_IN_H,
        TB_CONV_IN_W,
        TB_CONV_COUT
    );
    g_instr_mem[pc++] = tb_pack_u16x4(TB_CONV_KH, TB_CONV_KW, TB_CONV_STRIDE, 0);
#if GZY_ACCEL_TB_CONV_PREPACKED_WEIGHT
    g_instr_mem[pc++] = tb_pack_offsets(TB_A_CONV_IN, TB_B_CONV_W_PREPACKED);
#else
    g_instr_mem[pc++] = tb_pack_offsets(TB_A_CONV_IN, TB_B_CONV_W);
#endif
    g_instr_mem[pc++] = tb_pack_offsets(TB_A_CONV_IM2COL, TB_B_CONV_W_SCRATCH);
    g_instr_mem[pc++] = tb_pack_offsets(TB_C_CONV_OUT, TB_C_CONV_SCRATCH);

    g_instr_mem[pc++] = tb_pack_descriptor_header(
        ACCEL_OP_QKV_DDR,
        5,
        TB_OP_N,
        TB_OP_D,
        TB_OP_Q_SHIFT,
        0
    );
    g_instr_mem[pc++] = tb_pack_offsets(TB_A_X, TB_B_WQ);
    g_instr_mem[pc++] = tb_pack_offsets(TB_B_WK, TB_B_WV);
    g_instr_mem[pc++] = tb_pack_offsets(TB_A_Q, TB_B_KT);
    g_instr_mem[pc++] = tb_pack_offsets(TB_B_V, TB_C_QKV_SCRATCH);

    g_instr_mem[pc++] = tb_pack_descriptor_header(
        ACCEL_OP_ATTN_SCORE_DDR,
        3,
        TB_OP_N,
        TB_OP_D,
        0,
        0
    );
    g_instr_mem[pc++] = tb_pack_offsets(TB_A_Q, TB_B_KT);
    g_instr_mem[pc++] = tb_pack_offsets(TB_C_SCORE, 0);

    g_instr_mem[pc++] = tb_pack_descriptor_header(
        ACCEL_OP_ATTN_NORM,
        3,
        TB_OP_N,
        TB_OP_P_SHIFT,
        0,
        0
    );
    g_instr_mem[pc++] = tb_pack_offsets(TB_C_SCORE, TB_A_P);
    g_instr_mem[pc++] = 0;

    g_instr_mem[pc++] = tb_pack_descriptor_header(
        ACCEL_OP_ATTN_VALUE,
        3,
        TB_OP_N,
        TB_OP_D,
        0,
        0
    );
    g_instr_mem[pc++] = tb_pack_offsets(TB_A_P, TB_B_V);
    g_instr_mem[pc++] = tb_pack_offsets(TB_C_OUT, 0);

    g_instr_mem[pc++] = tb_pack_end_instr();
    instr_words = pc;
}

static gemm_acc_t tb_conv_ref(int co, int oh, int ow) {
    gemm_acc_t sum = 0;
    for (int ci = 0; ci < TB_CONV_CIN; ci++) {
        for (int r = 0; r < TB_CONV_KH; r++) {
            for (int s = 0; s < TB_CONV_KW; s++) {
                const int ih = oh * TB_CONV_STRIDE + r;
                const int iw = ow * TB_CONV_STRIDE + s;
                const int input_idx = TB_A_CONV_IN +
                    ci * TB_CONV_IN_H * TB_CONV_IN_W + ih * TB_CONV_IN_W + iw;
                const int weight_idx = TB_B_CONV_W +
                    ((co * TB_CONV_CIN + ci) * TB_CONV_KH + r) * TB_CONV_KW + s;
                sum += (gemm_acc_t)g_A_mem[input_idx] * (gemm_acc_t)g_B_mem[weight_idx];
            }
        }
    }
    return sum;
}

static int tb_check_conv_output() {
    int errors = 0;
    long long checksum = 0;

    for (int co = 0; co < TB_CONV_COUT; co++) {
        for (int oh = 0; oh < TB_CONV_OUT_H; oh++) {
            for (int ow = 0; ow < TB_CONV_OUT_W; ow++) {
                const int row = oh * TB_CONV_OUT_W + ow;
                const int got = (int)g_C_mem[TB_C_CONV_OUT + co * TB_CONV_OUT_HW + row];
                const int ref = (int)tb_conv_ref(co, oh, ow);
                checksum += (long long)got * (co * TB_CONV_OUT_HW + row + 1);
                if (got != ref) {
                    if (errors < 12) {
                        std::printf("[CONV2D][ERR] co=%d oh=%d ow=%d got %d expected %d\n",
                                    co, oh, ow, got, ref);
                    }
                    errors++;
                }
            }
        }
    }

    std::printf("[CONV2D] mismatch_count=%d checksum=%lld\n", errors, checksum);
    return errors;
}

static void tb_build_attention_reference(
    gemm_data_t q_ref[TB_OP_N][TB_OP_D],
    gemm_data_t kt_ref[TB_OP_D][TB_OP_N],
    gemm_data_t v_ref[TB_OP_N][TB_OP_D],
    gemm_acc_t score_ref[TB_OP_N][TB_OP_N],
    gemm_data_t p_ref[TB_OP_N][TB_OP_N],
    gemm_acc_t out_ref[TB_OP_N][TB_OP_D]
) {
    for (int i = 0; i < TB_OP_N; i++) {
        for (int d = 0; d < TB_OP_D; d++) {
            q_ref[i][d] = tb_quantize_i8(tb_qkv_sum(i, d, TB_B_WQ), TB_OP_Q_SHIFT);
            kt_ref[d][i] = tb_quantize_i8(tb_qkv_sum(i, d, TB_B_WK), TB_OP_Q_SHIFT);
            v_ref[i][d] = tb_quantize_i8(tb_qkv_sum(i, d, TB_B_WV), TB_OP_Q_SHIFT);
        }
    }

    for (int i = 0; i < TB_OP_N; i++) {
        for (int j = 0; j < TB_OP_N; j++) {
            gemm_acc_t sum = 0;
            for (int d = 0; d < TB_OP_D; d++) {
                sum += (gemm_acc_t)q_ref[i][d] * (gemm_acc_t)kt_ref[d][j];
            }
            score_ref[i][j] = sum;
        }
    }

    for (int i = 0; i < TB_OP_N; i++) {
        gemm_acc_t row_sum = 0;
        for (int j = 0; j < TB_OP_N; j++) {
            gemm_acc_t positive = score_ref[i][j] > 0 ? score_ref[i][j] : (gemm_acc_t)0;
            row_sum += positive;
        }

        for (int j = 0; j < TB_OP_N; j++) {
            gemm_acc_t positive = score_ref[i][j] > 0 ? score_ref[i][j] : (gemm_acc_t)0;
            if (row_sum == 0) {
                p_ref[i][j] = 0;
            } else {
#if GZY_ACCEL_TB_ATTN_NORM_POW2_APPROX
                const int norm_shift = tb_ceil_log2_positive(row_sum);
                gemm_acc_t scaled = (positive << TB_OP_P_SHIFT) >> norm_shift;
#else
                gemm_acc_t scaled = (positive << TB_OP_P_SHIFT) / row_sum;
#endif
                if (scaled > 127) {
                    scaled = 127;
                }
                p_ref[i][j] = (gemm_data_t)scaled;
            }
        }
    }

    for (int i = 0; i < TB_OP_N; i++) {
        for (int d = 0; d < TB_OP_D; d++) {
            gemm_acc_t sum = 0;
            for (int j = 0; j < TB_OP_N; j++) {
                sum += (gemm_acc_t)p_ref[i][j] * (gemm_acc_t)v_ref[j][d];
            }
            out_ref[i][d] = sum;
        }
    }
}

static int tb_check_attention_outputs() {
    static gemm_data_t q_ref[TB_OP_N][TB_OP_D];
    static gemm_data_t kt_ref[TB_OP_D][TB_OP_N];
    static gemm_data_t v_ref[TB_OP_N][TB_OP_D];
    static gemm_acc_t score_ref[TB_OP_N][TB_OP_N];
    static gemm_data_t p_ref[TB_OP_N][TB_OP_N];
    static gemm_acc_t out_ref[TB_OP_N][TB_OP_D];

    tb_build_attention_reference(q_ref, kt_ref, v_ref, score_ref, p_ref, out_ref);

    int errors = 0;
    int max_abs_error = 0;
    long long checksum = 0;

    for (int i = 0; i < TB_OP_N; i++) {
        for (int d = 0; d < TB_OP_D; d++) {
            const int got_q = (int)g_A_mem[TB_A_Q + i * TB_OP_D + d];
            const int ref_q = (int)q_ref[i][d];
            const int got_kt = (int)g_B_mem[TB_B_KT + d * TB_OP_N + i];
            const int ref_kt = (int)kt_ref[d][i];
            const int got_v = (int)g_B_mem[TB_B_V + i * TB_OP_D + d];
            const int ref_v = (int)v_ref[i][d];

            checksum += (long long)got_q * (i * TB_OP_D + d + 1);
            checksum += (long long)got_kt * (i * TB_OP_D + d + 3);
            checksum += (long long)got_v * (i * TB_OP_D + d + 5);

            const int diff_q = tb_abs_int(got_q - ref_q);
            const int diff_kt = tb_abs_int(got_kt - ref_kt);
            const int diff_v = tb_abs_int(got_v - ref_v);
            if (diff_q > max_abs_error) {
                max_abs_error = diff_q;
            }
            if (diff_kt > max_abs_error) {
                max_abs_error = diff_kt;
            }
            if (diff_v > max_abs_error) {
                max_abs_error = diff_v;
            }

            if (got_q != ref_q || got_kt != ref_kt || got_v != ref_v) {
                if (errors < 12) {
                    std::printf("[QKV][ERR] i=%d d=%d Q %d/%d KT %d/%d V %d/%d\n",
                                i, d, got_q, ref_q, got_kt, ref_kt, got_v, ref_v);
                }
                errors++;
            }
        }
    }

    for (int i = 0; i < TB_OP_N; i++) {
        for (int j = 0; j < TB_OP_N; j++) {
            const int got_score = (int)g_C_mem[TB_C_SCORE + i * TB_OP_N + j];
            const int ref_score = (int)score_ref[i][j];
            const int got_p = (int)g_A_mem[TB_A_P + i * TB_OP_N + j];
            const int ref_p = (int)p_ref[i][j];

            checksum += (long long)got_score * (i * TB_OP_N + j + 7);
            checksum += (long long)got_p * (i * TB_OP_N + j + 11);

            const int diff_score = tb_abs_int(got_score - ref_score);
            const int diff_p = tb_abs_int(got_p - ref_p);
            if (diff_score > max_abs_error) {
                max_abs_error = diff_score;
            }
            if (diff_p > max_abs_error) {
                max_abs_error = diff_p;
            }

            if (got_score != ref_score || got_p != ref_p) {
                if (errors < 12) {
                    std::printf("[ATTN][ERR] i=%d j=%d Score %d/%d P %d/%d\n",
                                i, j, got_score, ref_score, got_p, ref_p);
                }
                errors++;
            }
        }
    }

    for (int i = 0; i < TB_OP_N; i++) {
        for (int d = 0; d < TB_OP_D; d++) {
            const int got = (int)g_C_mem[TB_C_OUT + i * TB_OP_D + d];
            const int ref = (int)out_ref[i][d];
            const int diff = tb_abs_int(got - ref);
            checksum += (long long)got * (i * TB_OP_D + d + 13);
            if (diff > max_abs_error) {
                max_abs_error = diff;
            }
            if (got != ref) {
                if (errors < 12) {
                    std::printf("[ATTN_VALUE][ERR] i=%d d=%d got %d expected %d\n",
                                i, d, got, ref);
                }
                errors++;
            }
        }
    }

    std::printf("[ATTENTION_DESCRIPTOR] mismatch_count=%d max_abs_error=%d checksum=%lld\n",
                errors, max_abs_error, checksum);
    return errors;
}

int main() {
    tb_print_config("AXI_OPERATOR_DESCRIPTOR");

    int instr_words = 0;
    tb_init_operator_descriptor_case(instr_words);

    g_status[0] = accelerator_top_axi(
        g_instr_mem,
        g_A_mem,
        g_B_mem,
        g_C_mem,
        instr_words
    );

    const int conv_errors = tb_check_conv_output();
    const int attention_errors = tb_check_attention_outputs();

    std::printf("[AXI_OPERATOR_DESCRIPTOR] status=%d expected_status=5 instr_words=%d\n",
                g_status[0], instr_words);

    if (g_status[0] != 5) {
        std::printf("[AXI_OPERATOR_DESCRIPTOR] FAIL: unexpected status\n");
        return 1;
    }
    if (conv_errors != 0 || attention_errors != 0) {
        std::printf("[AXI_OPERATOR_DESCRIPTOR] FAIL: conv_errors=%d attention_errors=%d\n",
                    conv_errors, attention_errors);
        return 1;
    }

    std::printf("[AXI_OPERATOR_DESCRIPTOR] PASS full operator descriptor stream\n");
    return 0;
}
