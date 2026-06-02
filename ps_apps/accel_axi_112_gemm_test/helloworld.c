#include "platform.h"
#include "xaccelerator_top_axi_hw.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xparameters.h"
#include "xtime_l.h"

#define IP_BASEADDR XPAR_ACCELERATOR_TOP_AXI_0_S_AXI_CONTROL_BASEADDR

#define TEST_N 112
#define TEST_K 112
#define TEST_M 112
#define TEST_RUNS 3

#define MULTI_N 56
#define MULTI_K 56
#define MULTI_M 56

#define DDR_INSTR_ADDR 0x01010000U
#define DDR_A_ADDR     0x01020000U
#define DDR_B_ADDR     0x01030000U
#define DDR_C_ADDR     0x01040000U

#define A1_BASE_ELEMS 4096U
#define B1_BASE_ELEMS 4096U
#define C1_BASE_ELEMS 4096U

#define ACCEL_OP_END  0U
#define ACCEL_OP_GEMM 1U
#define ACCEL_BASE_UNIT 4096U

#define AP_CTRL_START 0x01U
#define AP_CTRL_DONE  0x02U
#define AP_CTRL_IDLE  0x04U
#define AP_CTRL_READY 0x08U

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

static u64 pack_gemm_instr(
    int n,
    int k,
    int m,
    u32 a_base_elems,
    u32 b_base_elems,
    u32 c_base_elems
)
{
    u64 word = 0;

    word |= (u64)ACCEL_OP_GEMM;
    word |= ((u64)(n - 1) & 0xFFFULL) << 8;
    word |= ((u64)(k - 1) & 0xFFFULL) << 20;
    word |= ((u64)(m - 1) & 0xFFFULL) << 32;
    word |= ((u64)(a_base_elems / ACCEL_BASE_UNIT) & 0x3FULL) << 44;
    word |= ((u64)(b_base_elems / ACCEL_BASE_UNIT) & 0x3FULL) << 50;
    word |= ((u64)(c_base_elems / ACCEL_BASE_UNIT) & 0x3FULL) << 56;

    return word;
}

static s8 gen_a(int tag, int i, int k)
{
    return (s8)(((i % 7) - 3) + ((k % 5) - 2) + tag);
}

static s8 gen_b(int tag, int k, int j)
{
    return (s8)(((j % 11) - 5) + ((k % 3) - 1) - tag);
}

static s32 golden_value(int tag, int k_dim, int i, int j)
{
    int k;
    s32 sum = 0;

    for (k = 0; k < k_dim; k++) {
        sum += (s32)gen_a(tag, i, k) * (s32)gen_b(tag, k, j);
    }

    return sum;
}

static void write_single_instr(int n, int k, int m)
{
    volatile u64 *instr = (volatile u64 *)DDR_INSTR_ADDR;

    instr[0] = pack_gemm_instr(n, k, m, 0U, 0U, 0U);
    instr[1] = ACCEL_OP_END;

    Xil_DCacheFlushRange((UINTPTR)instr, 2U * sizeof(u64));
}

static void write_multi_instr(void)
{
    volatile u64 *instr = (volatile u64 *)DDR_INSTR_ADDR;

    instr[0] = pack_gemm_instr(MULTI_N, MULTI_K, MULTI_M, 0U, 0U, 0U);
    instr[1] = pack_gemm_instr(
        MULTI_N,
        MULTI_K,
        MULTI_M,
        A1_BASE_ELEMS,
        B1_BASE_ELEMS,
        C1_BASE_ELEMS
    );
    instr[2] = ACCEL_OP_END;

    Xil_DCacheFlushRange((UINTPTR)instr, 3U * sizeof(u64));
}

static void fill_case(
    int tag,
    int n,
    int k_dim,
    int m,
    u32 a_base_elems,
    u32 b_base_elems,
    u32 c_base_elems
)
{
    volatile s8 *a = (volatile s8 *)DDR_A_ADDR;
    volatile s8 *b = (volatile s8 *)DDR_B_ADDR;
    volatile s32 *c = (volatile s32 *)DDR_C_ADDR;
    int i;
    int k;
    int j;

    for (i = 0; i < n; i++) {
        for (k = 0; k < k_dim; k++) {
            a[a_base_elems + i * k_dim + k] = gen_a(tag, i, k);
        }
    }

    for (k = 0; k < k_dim; k++) {
        for (j = 0; j < m; j++) {
            b[b_base_elems + k * m + j] = gen_b(tag, k, j);
        }
    }

    for (i = 0; i < n * m; i++) {
        c[c_base_elems + i] = (s32)(0x5A5A0000U + (u32)tag);
    }

    Xil_DCacheFlushRange((UINTPTR)(a + a_base_elems), n * k_dim * sizeof(s8));
    Xil_DCacheFlushRange((UINTPTR)(b + b_base_elems), k_dim * m * sizeof(s8));
    Xil_DCacheFlushRange((UINTPTR)(c + c_base_elems), n * m * sizeof(s32));
}

static int wait_done(u32 *last_ap_ctrl)
{
    int timeout = 100000000;
    u32 ap_ctrl = 0;

    while (timeout > 0) {
        ap_ctrl = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL);
        if ((ap_ctrl & AP_CTRL_DONE) != 0U) {
            *last_ap_ctrl = ap_ctrl;
            return 1;
        }
        timeout--;
    }

    *last_ap_ctrl = ap_ctrl;
    return 0;
}

static int run_accel(const char *label, u32 instr_num, u32 expected_return)
{
    XTime t_start;
    XTime t_end;
    u64 timer_ticks;
    u64 time_us;
    u32 ap_ctrl_before;
    u32 ap_ctrl_after;
    u32 ap_return;
    int done;

    xil_printf("%s\r\n", label);

    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL, 0U);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_INSTR_MEM_DATA, DDR_INSTR_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_A_MEM_DATA, DDR_A_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_B_MEM_DATA, DDR_B_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_C_MEM_DATA, DDR_C_ADDR);
    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_INSTR_NUM_DATA, instr_num);

    ap_ctrl_before = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL);
    xil_printf("AP_CTRL before start = 0x%08x\r\n", ap_ctrl_before);

    XTime_GetTime(&t_start);
    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL, AP_CTRL_START);
    done = wait_done(&ap_ctrl_after);
    XTime_GetTime(&t_end);

    timer_ticks = (u64)(t_end - t_start);
    time_us = (timer_ticks * 1000000ULL) / (u64)COUNTS_PER_SECOND;

    xil_printf("AP_CTRL after wait = 0x%08x\r\n", ap_ctrl_after);
    xil_printf("done=%d idle=%d ready=%d\r\n",
               done,
               (ap_ctrl_after & AP_CTRL_IDLE) ? 1 : 0,
               (ap_ctrl_after & AP_CTRL_READY) ? 1 : 0);
    print_u64_dec("timer_ticks = ", timer_ticks);
    print_u64_dec("approx_cpu_cycles = ", timer_ticks * 2ULL);
    print_u64_dec("time_us = ", time_us);

    if (!done) {
        xil_printf("GEMM sanity TIMEOUT\r\n");
        return 0;
    }

    ap_return = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_RETURN);
    xil_printf("ap_return = %d\r\n", ap_return);

    if (ap_return != expected_return) {
        xil_printf("ERR ap_return expected=%d got=%d\r\n", expected_return, ap_return);
        return 0;
    }

    return 1;
}

static int check_case(
    const char *label,
    int tag,
    int n,
    int k_dim,
    int m,
    u32 c_base_elems
)
{
    volatile s32 *c = (volatile s32 *)DDR_C_ADDR;
    int i;
    int j;
    int errors = 0;
    u32 checksum = 2166136261U;

    Xil_DCacheInvalidateRange((UINTPTR)(c + c_base_elems), n * m * sizeof(s32));

    for (i = 0; i < n; i++) {
        for (j = 0; j < m; j++) {
            const s32 got = c[c_base_elems + i * m + j];
            const s32 ref = golden_value(tag, k_dim, i, j);

            checksum ^= (u32)got + (u32)(i * m + j + 1);
            checksum *= 16777619U;

            if (got != ref) {
                if (errors < 8) {
                    xil_printf("%s ERR C[%d][%d] got=%d ref=%d\r\n",
                               label,
                               i,
                               j,
                               got,
                               ref);
                }
                errors++;
            }
        }
    }

    xil_printf("%s checksum32 = 0x%08x\r\n", label, checksum);
    xil_printf("%s mismatch_count = %d\r\n", label, errors);

    return errors == 0;
}

int main()
{
    int run;
    int pass_all = 1;

    init_platform();

    xil_printf("PS-PL-DDR GEMM extended sanity start\r\n");
    xil_printf("single shape N=%d K=%d M=%d repeat=%d\r\n", TEST_N, TEST_K, TEST_M, TEST_RUNS);
    xil_printf("multi shape  N=%d K=%d M=%d\r\n", MULTI_N, MULTI_K, MULTI_M);
    xil_printf("IP base    = 0x%08x\r\n", IP_BASEADDR);
    xil_printf("instr addr = 0x%08x\r\n", DDR_INSTR_ADDR);
    xil_printf("A addr     = 0x%08x\r\n", DDR_A_ADDR);
    xil_printf("B addr     = 0x%08x\r\n", DDR_B_ADDR);
    xil_printf("C addr     = 0x%08x\r\n", DDR_C_ADDR);
    xil_printf("A1 elems   = %d\r\n", A1_BASE_ELEMS);
    xil_printf("B1 elems   = %d\r\n", B1_BASE_ELEMS);
    xil_printf("C1 elems   = %d\r\n", C1_BASE_ELEMS);

    for (run = 0; run < TEST_RUNS; run++) {
        xil_printf("\r\n--- 112 single GEMM run %d/%d ---\r\n", run + 1, TEST_RUNS);
        write_single_instr(TEST_N, TEST_K, TEST_M);
        fill_case(run, TEST_N, TEST_K, TEST_M, 0U, 0U, 0U);

        if (!run_accel("run instruction stream: GEMM + END", 2U, 1U)) {
            pass_all = 0;
            continue;
        }

        if (!check_case("single112", run, TEST_N, TEST_K, TEST_M, 0U)) {
            pass_all = 0;
        }
    }

    xil_printf("\r\n--- multi instruction and multi buffer test ---\r\n");
    write_multi_instr();
    fill_case(3, MULTI_N, MULTI_K, MULTI_M, 0U, 0U, 0U);
    fill_case(4, MULTI_N, MULTI_K, MULTI_M, A1_BASE_ELEMS, B1_BASE_ELEMS, C1_BASE_ELEMS);

    if (!run_accel("run instruction stream: GEMM + GEMM + END", 3U, 2U)) {
        pass_all = 0;
    } else {
        if (!check_case("multi56_c0", 3, MULTI_N, MULTI_K, MULTI_M, 0U)) {
            pass_all = 0;
        }
        if (!check_case("multi56_c1", 4, MULTI_N, MULTI_K, MULTI_M, C1_BASE_ELEMS)) {
            pass_all = 0;
        }
    }

    if (pass_all) {
        xil_printf("\r\nPS-PL-DDR GEMM extended sanity PASS\r\n");
    } else {
        xil_printf("\r\nPS-PL-DDR GEMM extended sanity FAIL\r\n");
    }

    cleanup_platform();
    return pass_all ? 0 : 1;
}
