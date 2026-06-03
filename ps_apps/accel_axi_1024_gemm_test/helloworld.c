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

#define ACCEL_OP_END  0U
#define ACCEL_OP_GEMM 1U
#define ACCEL_BASE_UNIT 4096U

#define AP_CTRL_START 0x01U
#define AP_CTRL_DONE  0x02U
#define AP_CTRL_IDLE  0x04U
#define AP_CTRL_READY 0x08U

#define WAIT_TIMEOUT_US 30000000ULL

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

static u64 pack_gemm_instr(int n, int k, int m)
{
    u64 word = 0;

    word |= (u64)ACCEL_OP_GEMM;
    word |= ((u64)(n - 1) & 0xFFFULL) << 8;
    word |= ((u64)(k - 1) & 0xFFFULL) << 20;
    word |= ((u64)(m - 1) & 0xFFFULL) << 32;

    return word;
}

static int i_term(int i)
{
    return (i % 7) - 3;
}

static int k_a_term(int k)
{
    return (k % 5) - 2;
}

static int j_term(int j)
{
    return (j % 11) - 5;
}

static int k_b_term(int k)
{
    return (k % 3) - 1;
}

static s8 gen_a(int i, int k)
{
    return (s8)(i_term(i) + k_a_term(k));
}

static s8 gen_b(int k, int j)
{
    return (s8)(j_term(j) + k_b_term(k));
}

static void compute_k_sums(int k_dim, s64 *sum_f, s64 *sum_g, s64 *sum_fg)
{
    int k;

    *sum_f = 0;
    *sum_g = 0;
    *sum_fg = 0;

    for (k = 0; k < k_dim; k++) {
        const int f = k_a_term(k);
        const int g = k_b_term(k);
        *sum_f += f;
        *sum_g += g;
        *sum_fg += (s64)f * (s64)g;
    }
}

static s32 golden_value(int k_dim, int i, int j, s64 sum_f, s64 sum_g, s64 sum_fg)
{
    const s64 ai = i_term(i);
    const s64 bj = j_term(j);
    const s64 value =
        (s64)k_dim * ai * bj +
        ai * sum_g +
        bj * sum_f +
        sum_fg;

    return (s32)value;
}

static void write_instr(int n, int k, int m)
{
    volatile u64 *instr = (volatile u64 *)DDR_INSTR_ADDR;

    instr[0] = pack_gemm_instr(n, k, m);
    instr[1] = ACCEL_OP_END;

    Xil_DCacheFlushRange((UINTPTR)instr, 2U * sizeof(u64));
}

static void fill_buffers(int n, int k_dim, int m)
{
    volatile s8 *a = (volatile s8 *)DDR_A_ADDR;
    volatile s8 *b = (volatile s8 *)DDR_B_ADDR;
    volatile s32 *c = (volatile s32 *)DDR_C_ADDR;
    int i;
    int k;
    int j;

    for (i = 0; i < n; i++) {
        for (k = 0; k < k_dim; k++) {
            a[i * k_dim + k] = gen_a(i, k);
        }
    }

    for (k = 0; k < k_dim; k++) {
        for (j = 0; j < m; j++) {
            b[k * m + j] = gen_b(k, j);
        }
    }

    for (i = 0; i < n * m; i++) {
        c[i] = (s32)0x5A5A5A5A;
    }

    Xil_DCacheFlushRange((UINTPTR)a, n * k_dim * sizeof(s8));
    Xil_DCacheFlushRange((UINTPTR)b, k_dim * m * sizeof(s8));
    Xil_DCacheFlushRange((UINTPTR)c, n * m * sizeof(s32));
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

static int run_accel(int n, int k, int m, u64 *elapsed_us)
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
    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_INSTR_NUM_DATA, 2U);

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
        xil_printf("GEMM large TIMEOUT, N=%d K=%d M=%d\r\n", n, k, m);
        return 0;
    }

    ap_return = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_RETURN);
    xil_printf("ap_return = %d\r\n", ap_return);

    return ap_return == 1U;
}

static int check_output(int n, int k_dim, int m)
{
    volatile s32 *c = (volatile s32 *)DDR_C_ADDR;
    s64 sum_f;
    s64 sum_g;
    s64 sum_fg;
    int i;
    int j;
    int errors = 0;
    u32 checksum = 2166136261U;

    compute_k_sums(k_dim, &sum_f, &sum_g, &sum_fg);

    Xil_DCacheInvalidateRange((UINTPTR)c, n * m * sizeof(s32));

    for (i = 0; i < n; i++) {
        for (j = 0; j < m; j++) {
            const s32 got = c[i * m + j];
            const s32 ref = golden_value(k_dim, i, j, sum_f, sum_g, sum_fg);

            checksum ^= (u32)got + (u32)(i * m + j + 1);
            checksum *= 16777619U;

            if (got != ref) {
                if (errors < 8) {
                    xil_printf("ERR C[%d][%d] got=%d ref=%d\r\n", i, j, got, ref);
                }
                errors++;
            }
        }
    }

    xil_printf("checksum32 = 0x%08x\r\n", checksum);
    xil_printf("mismatch_count = %d\r\n", errors);

    return errors == 0;
}

static int run_case(int n, int k, int m)
{
    u64 elapsed_us = 0;
    int run_ok;
    int check_ok;

    xil_printf("\r\n--- GEMM large case N=%d K=%d M=%d ---\r\n", n, k, m);
    xil_printf("instr addr = 0x%08x\r\n", DDR_INSTR_ADDR);
    xil_printf("A addr     = 0x%08x\r\n", DDR_A_ADDR);
    xil_printf("B addr     = 0x%08x\r\n", DDR_B_ADDR);
    xil_printf("C addr     = 0x%08x\r\n", DDR_C_ADDR);

    write_instr(n, k, m);
    fill_buffers(n, k, m);

    run_ok = run_accel(n, k, m, &elapsed_us);
    if (!run_ok) {
        xil_printf("GEMM large run FAIL\r\n");
        return 0;
    }

    check_ok = check_output(n, k, m);
    if (check_ok) {
        xil_printf("GEMM large case PASS\r\n");
    } else {
        xil_printf("GEMM large case FAIL\r\n");
    }

    return check_ok;
}

int main()
{
    int pass_all = 1;

    init_platform();

    xil_printf("PS-PL-DDR GEMM 1024-capable sanity start\r\n");
    xil_printf("IP base = 0x%08x\r\n", IP_BASEADDR);
    xil_printf("This app runs 1008 full-block and 1024 boundary-block cases.\r\n");

    if (!run_case(1008, 1008, 1008)) {
        pass_all = 0;
    }

    if (!run_case(1024, 1024, 1024)) {
        pass_all = 0;
    }

    if (pass_all) {
        xil_printf("\r\nPS-PL-DDR GEMM 1024-capable sanity PASS\r\n");
    } else {
        xil_printf("\r\nPS-PL-DDR GEMM 1024-capable sanity FAIL\r\n");
    }

    cleanup_platform();
    return pass_all ? 0 : 1;
}
