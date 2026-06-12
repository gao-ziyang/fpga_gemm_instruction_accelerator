#include "platform.h"
#include "xaccelerator_top_axi_hw.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xparameters.h"
#include "xtime_l.h"

#define IP_BASEADDR XPAR_ACCELERATOR_TOP_AXI_0_S_AXI_CONTROL_BASEADDR

#define DDR_INSTR_ADDR 0x01010000U
#define DDR_A_ADDR     0x02000000U
#define DDR_B_ADDR     0x03000000U
#define DDR_C_ADDR     0x04000000U

#define ACCEL_OP_END            0U
#define ACCEL_OP_CONV2D         5U
#define ACCEL_OP_QKV_DDR        6U
#define ACCEL_OP_ATTN_SCORE_DDR 7U
#define ACCEL_OP_ATTN_NORM      8U
#define ACCEL_OP_ATTN_VALUE     9U

#define AP_CTRL_START 0x01U
#define AP_CTRL_DONE  0x02U
#define AP_CTRL_IDLE  0x04U
#define AP_CTRL_READY 0x08U

#define WAIT_TIMEOUT_US 30000000ULL

#define TB_OP_N       16
#define TB_OP_D       96
#define TB_OP_Q_SHIFT 5
#define TB_OP_P_SHIFT 6

#define TB_CONV_CIN    3
#define TB_CONV_IN_H   6
#define TB_CONV_IN_W   6
#define TB_CONV_COUT   4
#define TB_CONV_KH     3
#define TB_CONV_KW     3
#define TB_CONV_STRIDE 1
#define TB_CONV_OUT_H  ((TB_CONV_IN_H - TB_CONV_KH) / TB_CONV_STRIDE + 1)
#define TB_CONV_OUT_W  ((TB_CONV_IN_W - TB_CONV_KW) / TB_CONV_STRIDE + 1)
#define TB_CONV_OUT_HW (TB_CONV_OUT_H * TB_CONV_OUT_W)

#define TB_A_X           0
#define TB_A_Q           2048
#define TB_A_P           4096
#define TB_A_CONV_IN     4608
#define TB_A_CONV_IM2COL 5120

#define TB_B_WQ               0
#define TB_B_WK               12288
#define TB_B_WV               24576
#define TB_B_KT               36864
#define TB_B_V                40960
#define TB_B_CONV_W           45056
#define TB_B_CONV_W_SCRATCH   49152
#define TB_B_CONV_W_PREPACKED TB_B_CONV_W_SCRATCH

#define TB_C_QKV_SCRATCH  0
#define TB_C_SCORE        4096
#define TB_C_OUT          8192
#define TB_C_CONV_OUT     12288
#define TB_C_CONV_SCRATCH 16384

#define INSTR_WORDS_EXPECTED 21

static void write_reg64(u32 offset, u64 value)
{
    Xil_Out32(IP_BASEADDR + offset, (u32)value);
    Xil_Out32(IP_BASEADDR + offset + 4U, (u32)(value >> 32));
}

static void print_u64_dec(const char *label, u64 value)
{
    char buf[21];
    int pos = 20;

    buf[pos] = '\0';
    if (value == 0ULL) {
        xil_printf("%s0\r\n", label);
        return;
    }

    while (value > 0ULL && pos > 0) {
        buf[--pos] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    xil_printf("%s%s\r\n", label, &buf[pos]);
}

static u64 pack_descriptor_header(
    u32 opcode,
    u32 len,
    u32 arg0,
    u32 arg1,
    u32 arg2,
    u32 arg3
)
{
    u64 word = 0;

    word |= ((u64)opcode & 0xFFULL);
    word |= ((u64)len & 0xFFULL) << 8;
    word |= ((u64)arg0 & 0xFFFULL) << 16;
    word |= ((u64)arg1 & 0xFFFULL) << 28;
    word |= ((u64)arg2 & 0xFFFULL) << 40;
    word |= ((u64)arg3 & 0xFFFULL) << 52;
    return word;
}

static u64 pack_offsets(u32 lo, u32 hi)
{
    return ((u64)hi << 32) | (u64)lo;
}

static u64 pack_u16x4(u32 a, u32 b, u32 c, u32 d)
{
    u64 word = 0;

    word |= ((u64)a & 0xFFFFULL);
    word |= ((u64)b & 0xFFFFULL) << 16;
    word |= ((u64)c & 0xFFFFULL) << 32;
    word |= ((u64)d & 0xFFFFULL) << 48;
    return word;
}

static s8 pattern_i8(int index, int seed, int span)
{
    return (s8)(((index * 17 + seed * 23 + 11) % span) - (span / 2));
}

static s8 quantize_i8(s32 value, int shift)
{
    s32 shifted = value;

    if (shift > 0) {
        shifted = value >> shift;
    }
    if (shifted > 127) {
        return (s8)127;
    }
    if (shifted < -128) {
        return (s8)-128;
    }
    return (s8)shifted;
}

static int ceil_log2_positive(s32 value)
{
    u32 v = (u32)value;
    u32 tmp = v;
    int shift = 0;

    if (tmp > 0xFFFFU) {
        tmp >>= 16;
        shift += 16;
    }
    if (tmp > 0xFFU) {
        tmp >>= 8;
        shift += 8;
    }
    if (tmp > 0xFU) {
        tmp >>= 4;
        shift += 4;
    }
    if (tmp > 0x3U) {
        tmp >>= 2;
        shift += 2;
    }
    if (tmp > 0x1U) {
        shift += 1;
    }
    if ((v & (v - 1U)) != 0U) {
        shift += 1;
    }
    return shift;
}

static int abs_int(int x)
{
    return x < 0 ? -x : x;
}

static volatile s8 *a_mem(void)
{
    return (volatile s8 *)DDR_A_ADDR;
}

static volatile s8 *b_mem(void)
{
    return (volatile s8 *)DDR_B_ADDR;
}

static volatile s32 *c_mem(void)
{
    return (volatile s32 *)DDR_C_ADDR;
}

static void clear_regions(void)
{
    volatile s8 *a = a_mem();
    volatile s8 *b = b_mem();
    volatile s32 *c = c_mem();
    int i;

    for (i = 0; i < 8192; i++) {
        a[i] = 0;
    }
    for (i = 0; i < 65536; i++) {
        b[i] = 0;
    }
    for (i = 0; i < 32768; i++) {
        c[i] = 0;
    }
}

static void init_inputs(void)
{
    volatile s8 *a = a_mem();
    volatile s8 *b = b_mem();
    int i;
    int d;
    int h;
    int w;
    int c;
    int co;
    int ci;
    int r;
    int s;

    clear_regions();

    for (i = 0; i < TB_OP_N; i++) {
        for (d = 0; d < TB_OP_D; d++) {
            a[TB_A_X + i * TB_OP_D + d] = pattern_i8(i * TB_OP_D + d, 1, 9);
        }
    }

    for (d = 0; d < TB_OP_D; d++) {
        int j;
        for (j = 0; j < TB_OP_D; j++) {
            b[TB_B_WQ + d * TB_OP_D + j] = pattern_i8(d * TB_OP_D + j, 2, 7);
            b[TB_B_WK + d * TB_OP_D + j] = pattern_i8(d * TB_OP_D + j, 3, 7);
            b[TB_B_WV + d * TB_OP_D + j] = pattern_i8(d * TB_OP_D + j, 4, 7);
        }
    }

    for (c = 0; c < TB_CONV_CIN; c++) {
        for (h = 0; h < TB_CONV_IN_H; h++) {
            for (w = 0; w < TB_CONV_IN_W; w++) {
                const int idx = c * TB_CONV_IN_H * TB_CONV_IN_W + h * TB_CONV_IN_W + w;
                a[TB_A_CONV_IN + idx] = pattern_i8(idx, 5, 9);
            }
        }
    }

    for (co = 0; co < TB_CONV_COUT; co++) {
        for (ci = 0; ci < TB_CONV_CIN; ci++) {
            for (r = 0; r < TB_CONV_KH; r++) {
                for (s = 0; s < TB_CONV_KW; s++) {
                    const int idx = ((co * TB_CONV_CIN + ci) * TB_CONV_KH + r) *
                                    TB_CONV_KW + s;
                    b[TB_B_CONV_W + idx] = pattern_i8(idx, 6, 7);
                }
            }
        }
    }

    for (ci = 0; ci < TB_CONV_CIN; ci++) {
        for (r = 0; r < TB_CONV_KH; r++) {
            for (s = 0; s < TB_CONV_KW; s++) {
                const int col = (ci * TB_CONV_KH + r) * TB_CONV_KW + s;
                for (co = 0; co < TB_CONV_COUT; co++) {
                    const int src_idx = TB_B_CONV_W +
                        ((co * TB_CONV_CIN + ci) * TB_CONV_KH + r) * TB_CONV_KW + s;
                    b[TB_B_CONV_W_PREPACKED + col * TB_CONV_COUT + co] = b[src_idx];
                }
            }
        }
    }

    Xil_DCacheFlushRange(DDR_A_ADDR, 8192U);
    Xil_DCacheFlushRange(DDR_B_ADDR, 65536U);
    Xil_DCacheFlushRange(DDR_C_ADDR, 32768U * sizeof(s32));
}

static int write_descriptor_stream(void)
{
    volatile u64 *instr = (volatile u64 *)DDR_INSTR_ADDR;
    int pc = 0;

    instr[pc++] = pack_descriptor_header(ACCEL_OP_CONV2D, 6, 0, 0, 0, 0);
    instr[pc++] = pack_u16x4(TB_CONV_CIN, TB_CONV_IN_H, TB_CONV_IN_W, TB_CONV_COUT);
    instr[pc++] = pack_u16x4(TB_CONV_KH, TB_CONV_KW, TB_CONV_STRIDE, 0);
    instr[pc++] = pack_offsets(TB_A_CONV_IN, TB_B_CONV_W_PREPACKED);
    instr[pc++] = pack_offsets(TB_A_CONV_IM2COL, TB_B_CONV_W_SCRATCH);
    instr[pc++] = pack_offsets(TB_C_CONV_OUT, TB_C_CONV_SCRATCH);

    instr[pc++] = pack_descriptor_header(ACCEL_OP_QKV_DDR, 5, TB_OP_N, TB_OP_D, TB_OP_Q_SHIFT, 0);
    instr[pc++] = pack_offsets(TB_A_X, TB_B_WQ);
    instr[pc++] = pack_offsets(TB_B_WK, TB_B_WV);
    instr[pc++] = pack_offsets(TB_A_Q, TB_B_KT);
    instr[pc++] = pack_offsets(TB_B_V, TB_C_QKV_SCRATCH);

    instr[pc++] = pack_descriptor_header(ACCEL_OP_ATTN_SCORE_DDR, 3, TB_OP_N, TB_OP_D, 0, 0);
    instr[pc++] = pack_offsets(TB_A_Q, TB_B_KT);
    instr[pc++] = pack_offsets(TB_C_SCORE, 0);

    instr[pc++] = pack_descriptor_header(ACCEL_OP_ATTN_NORM, 3, TB_OP_N, TB_OP_P_SHIFT, 0, 0);
    instr[pc++] = pack_offsets(TB_C_SCORE, TB_A_P);
    instr[pc++] = 0ULL;

    instr[pc++] = pack_descriptor_header(ACCEL_OP_ATTN_VALUE, 3, TB_OP_N, TB_OP_D, 0, 0);
    instr[pc++] = pack_offsets(TB_A_P, TB_B_V);
    instr[pc++] = pack_offsets(TB_C_OUT, 0);

    instr[pc++] = ACCEL_OP_END;

    Xil_DCacheFlushRange(DDR_INSTR_ADDR, (u32)(pc * sizeof(u64)));
    return pc;
}

static s32 qkv_sum(int row, int col, int weight_base)
{
    volatile s8 *a = a_mem();
    volatile s8 *b = b_mem();
    s32 sum = 0;
    int d;

    for (d = 0; d < TB_OP_D; d++) {
        sum += (s32)a[TB_A_X + row * TB_OP_D + d] *
               (s32)b[weight_base + d * TB_OP_D + col];
    }
    return sum;
}

static s32 conv_ref(int co, int oh, int ow)
{
    volatile s8 *a = a_mem();
    volatile s8 *b = b_mem();
    s32 sum = 0;
    int ci;
    int r;
    int s;

    for (ci = 0; ci < TB_CONV_CIN; ci++) {
        for (r = 0; r < TB_CONV_KH; r++) {
            for (s = 0; s < TB_CONV_KW; s++) {
                const int ih = oh * TB_CONV_STRIDE + r;
                const int iw = ow * TB_CONV_STRIDE + s;
                const int input_idx = TB_A_CONV_IN +
                    ci * TB_CONV_IN_H * TB_CONV_IN_W + ih * TB_CONV_IN_W + iw;
                const int weight_idx = TB_B_CONV_W +
                    ((co * TB_CONV_CIN + ci) * TB_CONV_KH + r) * TB_CONV_KW + s;
                sum += (s32)a[input_idx] * (s32)b[weight_idx];
            }
        }
    }
    return sum;
}

static int check_conv_output(void)
{
    volatile s32 *c = c_mem();
    int errors = 0;
    u32 checksum = 2166136261U;
    int co;
    int oh;
    int ow;

    for (co = 0; co < TB_CONV_COUT; co++) {
        for (oh = 0; oh < TB_CONV_OUT_H; oh++) {
            for (ow = 0; ow < TB_CONV_OUT_W; ow++) {
                const int row = oh * TB_CONV_OUT_W + ow;
                const s32 got = c[TB_C_CONV_OUT + co * TB_CONV_OUT_HW + row];
                const s32 ref = conv_ref(co, oh, ow);
                checksum ^= (u32)got + (u32)(co * TB_CONV_OUT_HW + row + 1);
                checksum *= 16777619U;
                if (got != ref) {
                    if (errors < 8) {
                        xil_printf("[CONV2D][ERR] co=%d oh=%d ow=%d got=%d ref=%d\r\n",
                                   co, oh, ow, got, ref);
                    }
                    errors++;
                }
            }
        }
    }

    xil_printf("[CONV2D] checksum32=0x%08x mismatch_count=%d\r\n", checksum, errors);
    return errors;
}

static int check_attention_outputs(void)
{
    static s8 q_ref[TB_OP_N][TB_OP_D];
    static s8 kt_ref[TB_OP_D][TB_OP_N];
    static s8 v_ref[TB_OP_N][TB_OP_D];
    static s32 score_ref[TB_OP_N][TB_OP_N];
    static s8 p_ref[TB_OP_N][TB_OP_N];
    static s32 out_ref[TB_OP_N][TB_OP_D];
    volatile s8 *a = a_mem();
    volatile s8 *b = b_mem();
    volatile s32 *c = c_mem();
    int i;
    int d;
    int j;
    int errors = 0;
    int max_abs_error = 0;
    u32 checksum = 2166136261U;

    for (i = 0; i < TB_OP_N; i++) {
        for (d = 0; d < TB_OP_D; d++) {
            q_ref[i][d] = quantize_i8(qkv_sum(i, d, TB_B_WQ), TB_OP_Q_SHIFT);
            kt_ref[d][i] = quantize_i8(qkv_sum(i, d, TB_B_WK), TB_OP_Q_SHIFT);
            v_ref[i][d] = quantize_i8(qkv_sum(i, d, TB_B_WV), TB_OP_Q_SHIFT);
        }
    }

    for (i = 0; i < TB_OP_N; i++) {
        for (j = 0; j < TB_OP_N; j++) {
            s32 sum = 0;
            for (d = 0; d < TB_OP_D; d++) {
                sum += (s32)q_ref[i][d] * (s32)kt_ref[d][j];
            }
            score_ref[i][j] = sum;
        }
    }

    for (i = 0; i < TB_OP_N; i++) {
        s32 row_sum = 0;
        for (j = 0; j < TB_OP_N; j++) {
            row_sum += score_ref[i][j] > 0 ? score_ref[i][j] : 0;
        }
        for (j = 0; j < TB_OP_N; j++) {
            const s32 positive = score_ref[i][j] > 0 ? score_ref[i][j] : 0;
            if (row_sum == 0) {
                p_ref[i][j] = 0;
            } else {
                const int norm_shift = ceil_log2_positive(row_sum);
                s32 scaled = (positive << TB_OP_P_SHIFT) >> norm_shift;
                if (scaled > 127) {
                    scaled = 127;
                }
                p_ref[i][j] = (s8)scaled;
            }
        }
    }

    for (i = 0; i < TB_OP_N; i++) {
        for (d = 0; d < TB_OP_D; d++) {
            s32 sum = 0;
            for (j = 0; j < TB_OP_N; j++) {
                sum += (s32)p_ref[i][j] * (s32)v_ref[j][d];
            }
            out_ref[i][d] = sum;
        }
    }

    for (i = 0; i < TB_OP_N; i++) {
        for (d = 0; d < TB_OP_D; d++) {
            const int got_q = (int)a[TB_A_Q + i * TB_OP_D + d];
            const int ref_q = (int)q_ref[i][d];
            const int got_kt = (int)b[TB_B_KT + d * TB_OP_N + i];
            const int ref_kt = (int)kt_ref[d][i];
            const int got_v = (int)b[TB_B_V + i * TB_OP_D + d];
            const int ref_v = (int)v_ref[i][d];
            const int diff_q = abs_int(got_q - ref_q);
            const int diff_kt = abs_int(got_kt - ref_kt);
            const int diff_v = abs_int(got_v - ref_v);

            checksum ^= (u32)(got_q + got_kt + got_v + i + d);
            checksum *= 16777619U;
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
                if (errors < 8) {
                    xil_printf("[QKV][ERR] i=%d d=%d Q %d/%d KT %d/%d V %d/%d\r\n",
                               i, d, got_q, ref_q, got_kt, ref_kt, got_v, ref_v);
                }
                errors++;
            }
        }
    }

    for (i = 0; i < TB_OP_N; i++) {
        for (j = 0; j < TB_OP_N; j++) {
            const int got_score = (int)c[TB_C_SCORE + i * TB_OP_N + j];
            const int ref_score = (int)score_ref[i][j];
            const int got_p = (int)a[TB_A_P + i * TB_OP_N + j];
            const int ref_p = (int)p_ref[i][j];
            const int diff_score = abs_int(got_score - ref_score);
            const int diff_p = abs_int(got_p - ref_p);

            checksum ^= (u32)(got_score + got_p + i + j);
            checksum *= 16777619U;
            if (diff_score > max_abs_error) {
                max_abs_error = diff_score;
            }
            if (diff_p > max_abs_error) {
                max_abs_error = diff_p;
            }
            if (got_score != ref_score || got_p != ref_p) {
                if (errors < 8) {
                    xil_printf("[ATTN][ERR] i=%d j=%d Score %d/%d P %d/%d\r\n",
                               i, j, got_score, ref_score, got_p, ref_p);
                }
                errors++;
            }
        }
    }

    for (i = 0; i < TB_OP_N; i++) {
        for (d = 0; d < TB_OP_D; d++) {
            const int got = (int)c[TB_C_OUT + i * TB_OP_D + d];
            const int ref = (int)out_ref[i][d];
            const int diff = abs_int(got - ref);

            checksum ^= (u32)(got + i + d);
            checksum *= 16777619U;
            if (diff > max_abs_error) {
                max_abs_error = diff;
            }
            if (got != ref) {
                if (errors < 8) {
                    xil_printf("[ATTN_VALUE][ERR] i=%d d=%d got=%d ref=%d\r\n",
                               i, d, got, ref);
                }
                errors++;
            }
        }
    }

    xil_printf("[ATTENTION_DESCRIPTOR] checksum32=0x%08x mismatch_count=%d max_abs_error=%d\r\n",
               checksum, errors, max_abs_error);
    return errors;
}

static int wait_done(u32 *last_ap_ctrl)
{
    XTime t_start;
    XTime t_now;
    u32 ap_ctrl = 0;
    u64 elapsed_us = 0;

    XTime_GetTime(&t_start);
    while (elapsed_us < WAIT_TIMEOUT_US) {
        ap_ctrl = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL);
        if ((ap_ctrl & AP_CTRL_DONE) != 0U) {
            *last_ap_ctrl = ap_ctrl;
            return 1;
        }
        XTime_GetTime(&t_now);
        elapsed_us = ((u64)(t_now - t_start) * 1000000ULL) / (u64)COUNTS_PER_SECOND;
    }

    *last_ap_ctrl = ap_ctrl;
    return 0;
}

static int run_accel(int instr_words, u64 *elapsed_us)
{
    XTime t_start;
    XTime t_end;
    u64 timer_ticks;
    u32 ap_ctrl_before;
    u32 ap_ctrl_after;
    u32 ap_return;
    int done;

    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL, 0U);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_INSTR_MEM_DATA, DDR_INSTR_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_A_MEM_DATA, DDR_A_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_B_MEM_DATA, DDR_B_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_C_MEM_DATA, DDR_C_ADDR);
    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_INSTR_NUM_DATA, (u32)instr_words);

    ap_ctrl_before = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL);
    xil_printf("AP_CTRL before start = 0x%08x\r\n", ap_ctrl_before);

    XTime_GetTime(&t_start);
    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL, AP_CTRL_START);
    done = wait_done(&ap_ctrl_after);
    XTime_GetTime(&t_end);

    timer_ticks = (u64)(t_end - t_start);
    *elapsed_us = (timer_ticks * 1000000ULL) / (u64)COUNTS_PER_SECOND;

    xil_printf("AP_CTRL after wait = 0x%08x\r\n", ap_ctrl_after);
    xil_printf("done=%d idle=%d ready=%d\r\n",
               done,
               (ap_ctrl_after & AP_CTRL_IDLE) ? 1 : 0,
               (ap_ctrl_after & AP_CTRL_READY) ? 1 : 0);
    print_u64_dec("timer_ticks = ", timer_ticks);
    print_u64_dec("time_us = ", *elapsed_us);

    if (!done) {
        xil_printf("all-accelerator TIMEOUT\r\n");
        return -1;
    }

    ap_return = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_RETURN);
    xil_printf("ap_return = %d\r\n", ap_return);
    return (int)ap_return;
}

int main(void)
{
    int instr_words;
    int status;
    int conv_errors;
    int attn_errors;
    u64 elapsed_us = 0;

    init_platform();

    xil_printf("PS-PL-DDR all-accelerator TILE8 stage5 sanity start\r\n");
    xil_printf("IP base = 0x%08x\r\n", IP_BASEADDR);
    xil_printf("instr addr = 0x%08x\r\n", DDR_INSTR_ADDR);
    xil_printf("A addr     = 0x%08x\r\n", DDR_A_ADDR);
    xil_printf("B addr     = 0x%08x\r\n", DDR_B_ADDR);
    xil_printf("C addr     = 0x%08x\r\n", DDR_C_ADDR);

    init_inputs();
    instr_words = write_descriptor_stream();
    xil_printf("instr_words = %d expected = %d\r\n", instr_words, INSTR_WORDS_EXPECTED);

    status = run_accel(instr_words, &elapsed_us);
    if (status != 5) {
        xil_printf("all-accelerator run FAIL: status=%d expected=5\r\n", status);
        cleanup_platform();
        return 1;
    }

    Xil_DCacheInvalidateRange(DDR_A_ADDR, 8192U);
    Xil_DCacheInvalidateRange(DDR_B_ADDR, 65536U);
    Xil_DCacheInvalidateRange(DDR_C_ADDR, 32768U * sizeof(s32));

    conv_errors = check_conv_output();
    attn_errors = check_attention_outputs();

    if (conv_errors == 0 && attn_errors == 0) {
        xil_printf("PS-PL-DDR all-accelerator TILE8 stage5 sanity PASS\r\n");
    } else {
        xil_printf("PS-PL-DDR all-accelerator TILE8 stage5 sanity FAIL conv=%d attention=%d\r\n",
                   conv_errors, attn_errors);
    }

    cleanup_platform();
    return (conv_errors == 0 && attn_errors == 0) ? 0 : 1;
}
