#include "platform.h"
#include "xaccelerator_top_axi_hw.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xparameters.h"

#define IP_BASEADDR XPAR_ACCELERATOR_TOP_AXI_0_S_AXI_CONTROL_BASEADDR

#define TEST_N 112
#define TEST_K 112
#define TEST_M 112

#define DDR_INSTR_ADDR 0x01010000U
#define DDR_A_ADDR     0x01020000U
#define DDR_B_ADDR     0x01030000U
#define DDR_C_ADDR     0x01040000U

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

static u64 pack_gemm_instr(int n, int k, int m, int a_base, int b_base, int c_base)
{
    u64 word = 0;

    word |= (u64)ACCEL_OP_GEMM;
    word |= ((u64)(n - 1) & 0xFFFULL) << 8;
    word |= ((u64)(k - 1) & 0xFFFULL) << 20;
    word |= ((u64)(m - 1) & 0xFFFULL) << 32;
    word |= ((u64)(a_base / ACCEL_BASE_UNIT) & 0x3FULL) << 44;
    word |= ((u64)(b_base / ACCEL_BASE_UNIT) & 0x3FULL) << 50;
    word |= ((u64)(c_base / ACCEL_BASE_UNIT) & 0x3FULL) << 56;

    return word;
}

static s8 gen_a(int i, int k)
{
    return (s8)(((i % 7) - 3) + ((k % 5) - 2));
}

static s8 gen_b(int k, int j)
{
    return (s8)(((j % 11) - 5) + ((k % 3) - 1));
}

static s32 golden_value(int i, int j)
{
    int k;
    s32 sum = 0;

    for (k = 0; k < TEST_K; k++) {
        sum += (s32)gen_a(i, k) * (s32)gen_b(k, j);
    }

    return sum;
}

static void init_buffers(void)
{
    volatile u64 *instr = (volatile u64 *)DDR_INSTR_ADDR;
    volatile s8 *a = (volatile s8 *)DDR_A_ADDR;
    volatile s8 *b = (volatile s8 *)DDR_B_ADDR;
    volatile s32 *c = (volatile s32 *)DDR_C_ADDR;
    int i;
    int k;
    int j;

    instr[0] = pack_gemm_instr(TEST_N, TEST_K, TEST_M, 0, 0, 0);
    instr[1] = ACCEL_OP_END;

    for (i = 0; i < TEST_N; i++) {
        for (k = 0; k < TEST_K; k++) {
            a[i * TEST_K + k] = gen_a(i, k);
        }
    }

    for (k = 0; k < TEST_K; k++) {
        for (j = 0; j < TEST_M; j++) {
            b[k * TEST_M + j] = gen_b(k, j);
        }
    }

    for (i = 0; i < TEST_N * TEST_M; i++) {
        c[i] = 0;
    }

    Xil_DCacheFlushRange((UINTPTR)instr, 2U * sizeof(u64));
    Xil_DCacheFlushRange((UINTPTR)a, TEST_N * TEST_K * sizeof(s8));
    Xil_DCacheFlushRange((UINTPTR)b, TEST_K * TEST_M * sizeof(s8));
    Xil_DCacheFlushRange((UINTPTR)c, TEST_N * TEST_M * sizeof(s32));
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

static int check_output(void)
{
    volatile s32 *c = (volatile s32 *)DDR_C_ADDR;
    int i;
    int j;
    int errors = 0;
    long long checksum = 0;

    Xil_DCacheInvalidateRange((UINTPTR)c, TEST_N * TEST_M * sizeof(s32));

    for (i = 0; i < TEST_N; i++) {
        for (j = 0; j < TEST_M; j++) {
            const s32 got = c[i * TEST_M + j];
            const s32 ref = golden_value(i, j);

            checksum += (long long)got * (long long)(i * TEST_M + j + 1);

            if (got != ref) {
                if (errors < 8) {
                    xil_printf("ERR C[%d][%d] got=%d ref=%d\r\n", i, j, got, ref);
                }
                errors++;
            }
        }
    }

    xil_printf("checksum = %d\r\n", (int)checksum);
    xil_printf("mismatch_count = %d\r\n", errors);

    return errors == 0;
}

int main()
{
    u32 ap_ctrl_before;
    u32 ap_ctrl_after;
    u32 ap_return;
    int done;
    int pass;

    init_platform();

    xil_printf("PS-PL-DDR GEMM sanity start\r\n");
    xil_printf("shape N=%d K=%d M=%d\r\n", TEST_N, TEST_K, TEST_M);
    xil_printf("IP base    = 0x%08x\r\n", IP_BASEADDR);
    xil_printf("instr addr = 0x%08x\r\n", DDR_INSTR_ADDR);
    xil_printf("A addr     = 0x%08x\r\n", DDR_A_ADDR);
    xil_printf("B addr     = 0x%08x\r\n", DDR_B_ADDR);
    xil_printf("C addr     = 0x%08x\r\n", DDR_C_ADDR);

    init_buffers();

    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL, 0U);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_INSTR_MEM_DATA, DDR_INSTR_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_A_MEM_DATA, DDR_A_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_B_MEM_DATA, DDR_B_ADDR);
    write_reg64(XACCELERATOR_TOP_AXI_CONTROL_ADDR_C_MEM_DATA, DDR_C_ADDR);
    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_INSTR_NUM_DATA, 2U);

    ap_ctrl_before = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL);
    xil_printf("AP_CTRL before start = 0x%08x\r\n", ap_ctrl_before);

    Xil_Out32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_CTRL, AP_CTRL_START);

    done = wait_done(&ap_ctrl_after);
    xil_printf("AP_CTRL after wait = 0x%08x\r\n", ap_ctrl_after);
    xil_printf("done=%d idle=%d ready=%d\r\n",
               done,
               (ap_ctrl_after & AP_CTRL_IDLE) ? 1 : 0,
               (ap_ctrl_after & AP_CTRL_READY) ? 1 : 0);

    if (!done) {
        xil_printf("GEMM sanity TIMEOUT\r\n");
        cleanup_platform();
        return 1;
    }

    ap_return = Xil_In32(IP_BASEADDR + XACCELERATOR_TOP_AXI_CONTROL_ADDR_AP_RETURN);
    xil_printf("ap_return = %d\r\n", ap_return);

    pass = check_output();

    if (pass && ap_return == 1U) {
        xil_printf("PS-PL-DDR GEMM sanity PASS\r\n");
    } else {
        xil_printf("PS-PL-DDR GEMM sanity FAIL\r\n");
    }

    cleanup_platform();
    return (pass && ap_return == 1U) ? 0 : 1;
}
