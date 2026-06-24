#include "accelerator_instruction.h"

#include "gemm_scheduler.h"

#ifndef GZY_ACCEL_QKV_FUSED_STORE
#define GZY_ACCEL_QKV_FUSED_STORE 0
#endif

#ifndef GZY_ACCEL_CONV_FUSED_STORE
#define GZY_ACCEL_CONV_FUSED_STORE 0
#endif

#ifndef GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD
#define GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD 0
#endif

#ifndef GZY_ACCEL_CONV_PREPACKED_WEIGHT
#define GZY_ACCEL_CONV_PREPACKED_WEIGHT 0
#endif

#ifndef GZY_ACCEL_ATTN_NORM_POW2_APPROX
#define GZY_ACCEL_ATTN_NORM_POW2_APPROX 0
#endif

#ifndef GZY_ACCEL_CONV_IM2COL_SERIAL
#define GZY_ACCEL_CONV_IM2COL_SERIAL 0
#endif

#ifndef GZY_ACCEL_CONV_ASSUME_STRIDE1
#define GZY_ACCEL_CONV_ASSUME_STRIDE1 0
#endif

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

static int descriptor_len(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(15, 8);
}

static int descriptor_arg0(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(27, 16);
}

static int descriptor_arg1(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(39, 28);
}

static int descriptor_arg2(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(51, 40);
}

static int descriptor_offset_lo(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(31, 0);
}

static int descriptor_offset_hi(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(63, 32);
}

static int descriptor_u16_0(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(15, 0);
}

static int descriptor_u16_1(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(31, 16);
}

static int descriptor_u16_2(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(47, 32);
}

static int descriptor_u16_3(accel_instr_word_t word) {
#pragma HLS INLINE
    return (int)word.range(63, 48);
}

static gemm_data_t quantize_acc_to_i8(gemm_acc_t value, int shift) {
#pragma HLS INLINE
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

static int ceil_log2_positive(gemm_acc_t value) {
#pragma HLS INLINE
    ap_uint<32> v = (ap_uint<32>)value;
    ap_uint<32> tmp = v;
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

    if ((v & (v - 1)) != 0) {
        shift += 1;
    }
    return shift;
}

static void quantize_c_to_a(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    int rows,
    int cols,
    int c_base,
    int a_base,
    int shift
) {
#pragma HLS INLINE off
quant_c_a_i:
    for (int i = 0; i < rows; i++) {
    quant_c_a_j:
        for (int j = 0; j < cols; j++) {
#pragma HLS PIPELINE II=1
            A_mem[a_base + i * cols + j] =
                quantize_acc_to_i8(C_mem[c_base + i * cols + j], shift);
        }
    }
}

static void quantize_c_to_b(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    int rows,
    int cols,
    int c_base,
    int b_base,
    int shift
) {
#pragma HLS INLINE off
quant_c_b_i:
    for (int i = 0; i < rows; i++) {
    quant_c_b_j:
        for (int j = 0; j < cols; j++) {
#pragma HLS PIPELINE II=1
            B_mem[b_base + i * cols + j] =
                quantize_acc_to_i8(C_mem[c_base + i * cols + j], shift);
        }
    }
}

static void quantize_c_to_b_transpose(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    int rows,
    int cols,
    int c_base,
    int b_base,
    int shift
) {
#pragma HLS INLINE off
quant_c_bt_i:
    for (int i = 0; i < rows; i++) {
    quant_c_bt_j:
        for (int j = 0; j < cols; j++) {
#pragma HLS PIPELINE II=1
            B_mem[b_base + j * rows + i] =
                quantize_acc_to_i8(C_mem[c_base + i * cols + j], shift);
        }
    }
}

static void row_normalize_c_to_a(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    int N,
    int score_base,
    int p_base,
    int p_shift
) {
#pragma HLS INLINE off
row_norm_c_i:
    for (int i = 0; i < N; i++) {
        gemm_acc_t row_sum = 0;

    row_norm_c_sum_j:
        for (int j = 0; j < N; j++) {
#pragma HLS PIPELINE II=1
            gemm_acc_t value = C_mem[score_base + i * N + j];
            gemm_acc_t positive = value > 0 ? value : (gemm_acc_t)0;
            row_sum += positive;
        }

    row_norm_c_store_j:
        for (int j = 0; j < N; j++) {
#pragma HLS PIPELINE II=1
            gemm_acc_t value = C_mem[score_base + i * N + j];
            gemm_acc_t positive = value > 0 ? value : (gemm_acc_t)0;

            if (row_sum == 0) {
                A_mem[p_base + i * N + j] = 0;
            } else {
#if GZY_ACCEL_ATTN_NORM_POW2_APPROX
                const int norm_shift = ceil_log2_positive(row_sum);
                gemm_acc_t scaled = (positive << p_shift) >> norm_shift;
#else
                gemm_acc_t scaled = (positive << p_shift) / row_sum;
#endif
                if (scaled > 127) {
                    scaled = 127;
                }
                A_mem[p_base + i * N + j] = (gemm_data_t)scaled;
            }
        }
    }
}

static void conv2d_im2col_to_a(
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    int input_base,
    int im2col_base,
    int cin,
    int in_h,
    int in_w,
    int kh,
    int kw,
    int stride,
    int out_h,
    int out_w
) {
#pragma HLS INLINE off
conv_im2col_oh:
    for (int oh = 0; oh < out_h; oh++) {
#if GZY_ACCEL_CONV_IM2COL_SERIAL
#pragma HLS LOOP_FLATTEN off
#endif
    conv_im2col_ow:
        for (int ow = 0; ow < out_w; ow++) {
        conv_im2col_ci:
            for (int ci = 0; ci < cin; ci++) {
            conv_im2col_kh:
                for (int r = 0; r < kh; r++) {
                conv_im2col_kw:
                    for (int s = 0; s < kw; s++) {
#if !GZY_ACCEL_CONV_IM2COL_SERIAL
#pragma HLS PIPELINE II=1
#endif
                        const int row = oh * out_w + ow;
                        const int col = (ci * kh + r) * kw + s;
                        const int ih = oh * stride + r;
                        const int iw = ow * stride + s;
                        A_mem[im2col_base + row * (cin * kh * kw) + col] =
                            A_mem[input_base + ci * in_h * in_w + ih * in_w + iw];
                    }
                }
            }
        }
    }
}

static void conv2d_flatten_weight_to_b(
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    int weight_base,
    int weight_scratch_base,
    int cin,
    int kh,
    int kw,
    int cout
) {
#pragma HLS INLINE off
conv_w_co:
    for (int co = 0; co < cout; co++) {
    conv_w_ci:
        for (int ci = 0; ci < cin; ci++) {
        conv_w_kh:
            for (int r = 0; r < kh; r++) {
            conv_w_kw:
                for (int s = 0; s < kw; s++) {
#pragma HLS PIPELINE II=1
                    const int col = (ci * kh + r) * kw + s;
                    B_mem[weight_scratch_base + col * cout + co] =
                        B_mem[weight_base + ((co * cin + ci) * kh + r) * kw + s];
                }
            }
        }
    }
}

static void conv2d_store_channel_major(
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int c_scratch_base,
    int output_base,
    int out_hw,
    int cout
) {
#pragma HLS INLINE off
conv_store_row:
    for (int row = 0; row < out_hw; row++) {
    conv_store_co:
        for (int co = 0; co < cout; co++) {
#pragma HLS PIPELINE II=1
            C_mem[output_base + co * out_hw + row] =
                C_mem[c_scratch_base + row * cout + co];
        }
    }
}

static bool is_descriptor_opcode(ap_uint<8> opcode) {
#pragma HLS INLINE
    return opcode == ACCEL_OP_CONV2D ||
           opcode == ACCEL_OP_QKV_DDR ||
           opcode == ACCEL_OP_ATTN_SCORE_DDR ||
           opcode == ACCEL_OP_ATTN_NORM ||
           opcode == ACCEL_OP_ATTN_VALUE;
}

static const int SHARED_GEMM_NONE = 0;
static const int SHARED_GEMM_DIRECT = 1;
static const int SHARED_GEMM_QKV_LEGACY = 2;
static const int SHARED_GEMM_CONV2D = 3;
static const int SHARED_GEMM_QKV_DDR = 4;
static const int SHARED_GEMM_ATTN_SCORE = 5;
static const int SHARED_GEMM_ATTN_VALUE = 6;

int execute_instruction_stream(
    accel_instr_word_t instr_mem[ACCEL_MAX_INSTR],
    gemm_data_t A_mem[ACCEL_A_ELEMS],
    gemm_data_t B_mem[ACCEL_B_ELEMS],
    gemm_acc_t C_mem[ACCEL_C_ELEMS],
    int instr_num
) {
#pragma HLS INLINE off
#pragma HLS ALLOCATION function instances=gemm_scheduler limit=1
    int executed = 0;
    bool done = false;
    int pc = 0;

instr_loop:
    for (int issue = 0; issue < ACCEL_MAX_INSTR; issue++) {
        if (pc < instr_num && !done) {//instr_mem类型是accel_instr_word_t，即无符号64bit
            //instr_mem本身是由AXILite直接得到的（PS写入）
            //instr_mem是地址放在DDR，用解码指令硬件去读，得到具体指令如gemm或者end即变量instr。

            decoded_instr_t instr = decode_instruction(instr_mem[pc]);
            int len = 1;
            int gemm_kind = SHARED_GEMM_NONE;
            int gemm_count = 0;

            int shared_N = 0;
            int shared_K = 0;
            int shared_M = 0;
            int shared_a_base = 0;
            int shared_b_base = 0;
            int shared_c_base = 0;

            int q_shift = 0;
            int qkv_x_base = 0;
            int qkv_wq_base = 0;
            int qkv_wk_base = 0;
            int qkv_wv_base = 0;
            int qkv_q_base = 0;
            int qkv_kt_base = 0;
            int qkv_v_base = 0;
            int qkv_c_scratch_base = 0;

            int conv_input_base = 0;
            int conv_weight_base = 0;
            int conv_im2col_base = 0;
            int conv_weight_scratch_base = 0;
            int conv_output_base = 0;
            int conv_c_scratch_base = 0;
            int conv_out_hw = 0;
            int conv_cout = 0;
            int conv_cin = 0;
            int conv_kh = 0;
            int conv_kw = 0;

            int attn_p_shift = 0;
            int attn_score_base = 0;
            int attn_p_base = 0;
            int attn_v_base = 0;
            int attn_out_base = 0;

            if (instr.opcode == ACCEL_OP_END) {
                done = true;
                pc++;
            } else if (instr.opcode == ACCEL_OP_GEMM) {
                gemm_kind = SHARED_GEMM_DIRECT;
                gemm_count = 1;
                shared_N = instr.N;
                shared_K = instr.K;
                shared_M = instr.M;
                shared_a_base = instr.a_base;
                shared_b_base = instr.b_base;
                shared_c_base = instr.c_base;
            } else if (instr.opcode == ACCEL_OP_QKV) {
                gemm_kind = SHARED_GEMM_QKV_LEGACY;
                gemm_count = 3;
                shared_N = instr.N;
                shared_K = instr.K;
                shared_M = instr.M;
                shared_a_base = instr.a_base;
                shared_b_base = instr.b_base;
                shared_c_base = instr.c_base;
            } else if (instr.opcode == ACCEL_OP_ATTN_SCORE) {
                gemm_kind = SHARED_GEMM_DIRECT;
                gemm_count = 1;
                shared_N = instr.N;
                shared_K = instr.K;
                shared_M = instr.M;
                shared_a_base = instr.a_base;
                shared_b_base = instr.b_base;
                shared_c_base = instr.c_base;
            } else if (instr.opcode == ACCEL_OP_CONV_GEMM) {
                gemm_kind = SHARED_GEMM_DIRECT;
                gemm_count = 1;
                shared_N = instr.N;
                shared_K = instr.K;
                shared_M = instr.M;
                shared_a_base = instr.a_base;
                shared_b_base = instr.b_base;
                shared_c_base = instr.c_base;
            } else if (is_descriptor_opcode(instr.opcode)) {
                len = descriptor_len(instr_mem[pc]);
                if (len <= 0 || pc + len > instr_num) {
                    return -2;
                }

                if (instr.opcode == ACCEL_OP_CONV2D) {
                    if (len != 6) {
                        return -3;
                    }

                    const accel_instr_word_t shape1 = instr_mem[pc + 1];
                    const accel_instr_word_t shape2 = instr_mem[pc + 2];
                    const int cin = descriptor_u16_0(shape1);
                    const int in_h = descriptor_u16_1(shape1);
                    const int in_w = descriptor_u16_2(shape1);
                    const int cout = descriptor_u16_3(shape1);
                    const int kh = descriptor_u16_0(shape2);
                    const int kw = descriptor_u16_1(shape2);
#if GZY_ACCEL_CONV_ASSUME_STRIDE1
                    const int encoded_stride = descriptor_u16_2(shape2);
                    const int stride = 1;
#else
                    const int stride = descriptor_u16_2(shape2);
#endif

                    conv_input_base = descriptor_offset_lo(instr_mem[pc + 3]);
                    conv_weight_base = descriptor_offset_hi(instr_mem[pc + 3]);
                    conv_im2col_base = descriptor_offset_lo(instr_mem[pc + 4]);
                    conv_weight_scratch_base = descriptor_offset_hi(instr_mem[pc + 4]);
                    conv_output_base = descriptor_offset_lo(instr_mem[pc + 5]);
                    conv_c_scratch_base = descriptor_offset_hi(instr_mem[pc + 5]);

                    if (cin <= 0 || in_h <= 0 || in_w <= 0 || cout <= 0 ||
                        kh <= 0 || kw <= 0 || stride <= 0 || in_h < kh || in_w < kw
#if GZY_ACCEL_CONV_ASSUME_STRIDE1
                        || encoded_stride != 1
#endif
                    ) {
                        return -10;
                    }

#if GZY_ACCEL_CONV_ASSUME_STRIDE1
                    const int conv_out_h = in_h - kh + 1;
                    const int conv_out_w = in_w - kw + 1;
#else
                    const int conv_out_h = (in_h - kh) / stride + 1;
                    const int conv_out_w = (in_w - kw) / stride + 1;
#endif
                    conv_out_hw = conv_out_h * conv_out_w;
                    conv_cout = cout;
                    conv_cin = cin;
                    conv_kh = kh;
                    conv_kw = kw;

                    conv2d_im2col_to_a(
                        A_mem,
                        conv_input_base,
                        conv_im2col_base,
                        cin,
                        in_h,
                        in_w,
                        kh,
                        kw,
                        stride,
                        conv_out_h,
                        conv_out_w
                    );
#if GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD
                    // GEMM load B reads the original Conv2D weight layout directly.
#elif GZY_ACCEL_CONV_PREPACKED_WEIGHT
                    // B_mem already stores Conv2D weights in GEMM KxM layout.
#else
                    conv2d_flatten_weight_to_b(
                        B_mem,
                        conv_weight_base,
                        conv_weight_scratch_base,
                        cin,
                        kh,
                        kw,
                        cout
                    );
#endif

                    gemm_kind = SHARED_GEMM_CONV2D;
                    gemm_count = 1;
                    shared_N = conv_out_hw;
                    shared_K = cin * kh * kw;
                    shared_M = cout;
                    shared_a_base = conv_im2col_base;
#if GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD || GZY_ACCEL_CONV_PREPACKED_WEIGHT
                    shared_b_base = conv_weight_base;
#else
                    shared_b_base = conv_weight_scratch_base;
#endif
#if GZY_ACCEL_CONV_FUSED_STORE
                    shared_c_base = conv_output_base;
#else
                    shared_c_base = conv_c_scratch_base;
#endif
                } else if (instr.opcode == ACCEL_OP_QKV_DDR) {
                    if (len != 5) {
                        return -4;
                    }

                    const accel_instr_word_t header = instr_mem[pc];
                    shared_N = descriptor_arg0(header);
                    shared_K = descriptor_arg1(header);
                    shared_M = shared_K;
                    q_shift = descriptor_arg2(header);

                    qkv_x_base = descriptor_offset_lo(instr_mem[pc + 1]);
                    qkv_wq_base = descriptor_offset_hi(instr_mem[pc + 1]);
                    qkv_wk_base = descriptor_offset_lo(instr_mem[pc + 2]);
                    qkv_wv_base = descriptor_offset_hi(instr_mem[pc + 2]);
                    qkv_q_base = descriptor_offset_lo(instr_mem[pc + 3]);
                    qkv_kt_base = descriptor_offset_hi(instr_mem[pc + 3]);
                    qkv_v_base = descriptor_offset_lo(instr_mem[pc + 4]);
                    qkv_c_scratch_base = descriptor_offset_hi(instr_mem[pc + 4]);

                    if (shared_N <= 0 || shared_K <= 0) {
                        return -11;
                    }

                    gemm_kind = SHARED_GEMM_QKV_DDR;
                    gemm_count = 3;
                } else if (instr.opcode == ACCEL_OP_ATTN_SCORE_DDR) {
                    if (len != 3) {
                        return -5;
                    }

                    const accel_instr_word_t header = instr_mem[pc];
                    shared_N = descriptor_arg0(header);
                    shared_K = descriptor_arg1(header);
                    shared_M = shared_N;
                    shared_a_base = descriptor_offset_lo(instr_mem[pc + 1]);
                    shared_b_base = descriptor_offset_hi(instr_mem[pc + 1]);
                    shared_c_base = descriptor_offset_lo(instr_mem[pc + 2]);

                    if (shared_N <= 0 || shared_K <= 0) {
                        return -12;
                    }

                    gemm_kind = SHARED_GEMM_ATTN_SCORE;
                    gemm_count = 1;
                } else if (instr.opcode == ACCEL_OP_ATTN_NORM) {
                    if (len != 3) {
                        return -6;
                    }

                    const accel_instr_word_t header = instr_mem[pc];
                    shared_N = descriptor_arg0(header);
                    attn_p_shift = descriptor_arg1(header);
                    attn_score_base = descriptor_offset_lo(instr_mem[pc + 1]);
                    attn_p_base = descriptor_offset_hi(instr_mem[pc + 1]);

                    if (shared_N <= 0) {
                        return -13;
                    }

                    row_normalize_c_to_a(
                        C_mem,
                        A_mem,
                        shared_N,
                        attn_score_base,
                        attn_p_base,
                        attn_p_shift
                    );
                    executed++;
                    pc += len;
                    continue;
                } else if (instr.opcode == ACCEL_OP_ATTN_VALUE) {
                    if (len != 3) {
                        return -7;
                    }

                    const accel_instr_word_t header = instr_mem[pc];
                    shared_N = descriptor_arg0(header);
                    shared_K = shared_N;
                    shared_M = descriptor_arg1(header);
                    attn_p_base = descriptor_offset_lo(instr_mem[pc + 1]);
                    attn_v_base = descriptor_offset_hi(instr_mem[pc + 1]);
                    attn_out_base = descriptor_offset_lo(instr_mem[pc + 2]);

                    if (shared_N <= 0 || shared_M <= 0) {
                        return -14;
                    }

                    gemm_kind = SHARED_GEMM_ATTN_VALUE;
                    gemm_count = 1;
                    shared_a_base = attn_p_base;
                    shared_b_base = attn_v_base;
                    shared_c_base = attn_out_base;
                }
            } else {
                return -1;
            }

        shared_gemm_stage_loop:
            for (int stage = 0; stage < 3; stage++) {
                if (stage < gemm_count) {
                    int gemm_N = shared_N;
                    int gemm_K = shared_K;
                    int gemm_M = shared_M;
                    int gemm_a_base = shared_a_base;
                    int gemm_b_base = shared_b_base;
                    int gemm_c_base = shared_c_base;
                    int gemm_store_mode = GEMM_STORE_ACC_C;
                    int gemm_store_i8_base = 0;
                    int gemm_store_i8_shift = 0;
                    int gemm_load_b_mode = GEMM_LOAD_B_ROW_MAJOR;
                    int gemm_conv_cin = 0;
                    int gemm_conv_kh = 0;
                    int gemm_conv_kw = 0;

                    if (gemm_kind == SHARED_GEMM_CONV2D) {
#if GZY_ACCEL_CONV_DIRECT_WEIGHT_LOAD
                        gemm_load_b_mode = GEMM_LOAD_B_CONV_WEIGHT;
                        gemm_conv_cin = conv_cin;
                        gemm_conv_kh = conv_kh;
                        gemm_conv_kw = conv_kw;
#endif
#if GZY_ACCEL_CONV_FUSED_STORE
                        gemm_store_mode = GEMM_STORE_ACC_C_TRANSPOSE;
#endif
                    } else if (gemm_kind == SHARED_GEMM_QKV_LEGACY) {
                        gemm_b_base = shared_b_base + stage * ACCEL_QKV_B_STRIDE;
                        gemm_c_base = shared_c_base + stage * ACCEL_QKV_C_STRIDE;
                    } else if (gemm_kind == SHARED_GEMM_QKV_DDR) {
                        gemm_a_base = qkv_x_base;
                        gemm_c_base = qkv_c_scratch_base;
                        if (stage == 0) {
                            gemm_b_base = qkv_wq_base;
#if GZY_ACCEL_QKV_FUSED_STORE
                            gemm_store_mode = GEMM_STORE_I8_A;
                            gemm_store_i8_base = qkv_q_base;
                            gemm_store_i8_shift = q_shift;
#endif
                        } else if (stage == 1) {
                            gemm_b_base = qkv_wk_base;
#if GZY_ACCEL_QKV_FUSED_STORE
                            gemm_store_mode = GEMM_STORE_I8_B_TRANSPOSE;
                            gemm_store_i8_base = qkv_kt_base;
                            gemm_store_i8_shift = q_shift;
#endif
                        } else {
                            gemm_b_base = qkv_wv_base;
#if GZY_ACCEL_QKV_FUSED_STORE
                            gemm_store_mode = GEMM_STORE_I8_B;
                            gemm_store_i8_base = qkv_v_base;
                            gemm_store_i8_shift = q_shift;
#endif
                        }
                    }

                    gemm_scheduler(
                        A_mem,
                        B_mem,
                        C_mem,
                        gemm_N,
                        gemm_K,
                        gemm_M,
                        gemm_a_base,
                        gemm_b_base,
                        gemm_c_base,
                        gemm_load_b_mode,
                        gemm_conv_cin,
                        gemm_conv_kh,
                        gemm_conv_kw,
                        gemm_store_mode,
                        gemm_store_i8_base,
                        gemm_store_i8_shift
                    );

                    if (gemm_kind == SHARED_GEMM_CONV2D) {
#if GZY_ACCEL_CONV_FUSED_STORE
                        // Conv2D output was written directly in channel-major order.
#else
                        conv2d_store_channel_major(
                            C_mem,
                            conv_c_scratch_base,
                            conv_output_base,
                            conv_out_hw,
                            conv_cout
                        );
#endif
                    } else if (gemm_kind == SHARED_GEMM_QKV_DDR) {
#if GZY_ACCEL_QKV_FUSED_STORE
                        // Q/K^T/V were written directly by gemm_scheduler store mode.
#else
                        if (stage == 0) {
                            quantize_c_to_a(
                                C_mem,
                                A_mem,
                                shared_N,
                                shared_M,
                                qkv_c_scratch_base,
                                qkv_q_base,
                                q_shift
                            );
                        } else if (stage == 1) {
                            quantize_c_to_b_transpose(
                                C_mem,
                                B_mem,
                                shared_N,
                                shared_M,
                                qkv_c_scratch_base,
                                qkv_kt_base,
                                q_shift
                            );
                        } else {
                            quantize_c_to_b(
                                C_mem,
                                B_mem,
                                shared_N,
                                shared_M,
                                qkv_c_scratch_base,
                                qkv_v_base,
                                q_shift
                            );
                        }
#endif
                    }
                }
            }

            if (gemm_kind != SHARED_GEMM_NONE) {
                executed++;
                pc += len;
            }
        }
    }

    return executed;
}
