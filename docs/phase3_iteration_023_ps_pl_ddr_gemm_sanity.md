# Phase 3 Iteration 023：PS-PL-DDR GEMM sanity test 跑通

## 我这一版想解决什么

前面第 1 层已经验证了：

```text
PS -> DDR
```

第 2 层已经验证了：

```text
PS -> AXI-Lite -> accelerator_top_axi control register
```

这一版要验证第 3 层，也就是完整的最小 GEMM 上板链路：

```text
PS 准备 A/B/instruction/C buffer 到 DDR
PS 通过 AXI-Lite 配置 accelerator_top_axi
PL 通过 m_axi/HP 口访问 DDR
PL 执行 GEMM
PL 把 C 写回 DDR
PS invalidate cache 后读回 C
PS 和 CPU golden 对比
```

这一步如果通过，就说明 PL 不只是能被配置寄存器访问，而是真的开始从 DDR 读数据、计算、写回结果。

## 这次新建的 application

在 Vitis 里新建：

```text
accel_axi_112_gemm_test
```

模板仍然选择 `Hello World`，然后替换 `src/helloworld.c`。

linker script 继续保持所有关键段放在：

```text
ps7_ddr_0
```

这样 ELF 下载和运行地址仍在 DDR：

```text
section, .text: 0x00100000 - 0x00102acb
section, .data: 0x00102dc0 - 0x0010322f
section, .bss: 0x00108008 - 0x0010802f
section, .heap: 0x00108030 - 0x0010a02f
section, .stack: 0x0010a030 - 0x0010d82f
```

## 这次测试的规模

第一次没有直接跑 HLS 脚本里 112 x 112 x 112 的大 GEMM，而是先跑：

```text
N = 4
K = 4
M = 4
```

原因是这一层的目标不是性能，而是链路 sanity：

```text
AXI-Lite start 是否有效
PL m_axi 是否能从 DDR 读 A/B/instruction
PL 是否能写回 C
PS cache flush/invalidate 是否处理对
结果是否和 golden 对齐
```

小矩阵更容易定位问题。如果一开始跑大矩阵，失败时会很难判断是 HP/DDR、cache、指令、地址、计算还是等待时间的问题。

## DDR buffer 地址

这版使用固定 DDR 地址：

```text
instr = 0x01010000
A     = 0x01020000
B     = 0x01030000
C     = 0x01040000
```

这些地址避开 ELF 本身的 `.text/.data/.bss/.heap/.stack` 区域。程序先在 PS 端写入：

```text
instr[0] = GEMM instruction
instr[1] = END instruction
A        = int8 input matrix
B        = int8 weight matrix
C        = zeroed int32 output matrix
```

然后对 instruction/A/B/C 做 `Xil_DCacheFlushRange`，确保 PL 从 DDR 读到的是 PS 已经写进去的新数据。

PL 计算结束后，PS 对 C 做 `Xil_DCacheInvalidateRange`，确保 PS 读到的是 PL 写回 DDR 的新结果，而不是 CPU cache 里的旧值。

## AXI-Lite 配置

程序通过 AXI-Lite 写入 HLS IP 的控制寄存器：

```text
instr_mem = 0x01010000
A_mem     = 0x01020000
B_mem     = 0x01030000
C_mem     = 0x01040000
instr_num = 2
```

然后写 `AP_CTRL.ap_start = 1` 启动 IP。

这里 `instr_num = 2` 表示：

```text
第 0 条：GEMM
第 1 条：END
```

## XSCT 下载运行

这次继续用脚本下载 ELF：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct_run_gemm_test.tcl
```

脚本做的事情是：

```text
connect
targets
选择 ARM Cortex-A9 #0
stop
ps7_init
ps7_post_config
dow accel_axi_112_gemm_test.elf
con
```

本次 XSCT 输出中，GEMM test ELF 成功下载：

```text
Downloading Program -- C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_gemm_test/Debug/accel_axi_112_gemm_test.elf
section, .text: 0x00100000 - 0x00102acb
section, .data: 0x00102dc0 - 0x0010322f
section, .bss: 0x00108008 - 0x0010802f
section, .heap: 0x00108030 - 0x0010a02f
section, .stack: 0x0010a030 - 0x0010d82f
Setting PC to Program Start Address 0x00100000
Successfully downloaded C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_gemm_test/Debug/accel_axi_112_gemm_test.elf
Info: ARM Cortex-A9 MPCore #0 (target 2) Running
```

虽然 `targets` 里仍出现过：

```text
whole scan chain (DR shift output all ones)
```

但这次 A9 #0 能成功 `stop`，ELF 也能成功 `dow`，说明当时 PS debug 链路对这次下载运行是可用的。

## 串口结果

上位机串口输出：

```text
PS-PL-DDR GEMM sanity start
shape N=4 K=4 M=4
IP base    = 0x40000000
instr addr = 0x01010000
A addr     = 0x01020000
B addr     = 0x01030000
C addr     = 0x01040000
AP_CTRL before start = 0x00000004
AP_CTRL after wait = 0x00000006
done=1 idle=1 ready=0
ap_return = 1
checksum = 2788
mismatch_count = 0
PS-PL-DDR GEMM sanity PASS
```

这里每一项的含义：

```text
AP_CTRL before start = 0x00000004
  IP 启动前处于 idle。

AP_CTRL after wait = 0x00000006
  bit1 done = 1，bit2 idle = 1，说明 IP 已经完成并回到空闲。

ap_return = 1
  execute_instruction_stream 返回执行了 1 条 GEMM 指令。

mismatch_count = 0
  PL 写回 DDR 的 C 矩阵和 PS 端 CPU golden 完全一致。

PS-PL-DDR GEMM sanity PASS
  第 3 层完整链路通过。
```

`ready=0` 不是本次失败点，因为 `done=1`、`idle=1`、`ap_return=1`、`mismatch_count=0` 已经说明计算完成且结果正确。HLS 的 `ap_ready` 有时是握手瞬时状态，不如 done/idle/result check 重要。

## 放大到 16x16x16

4x4 通过后，我把同一个 PS application 改成：

```text
N = 16
K = 16
M = 16
```

重新 build ELF 后，用：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct_run_gemm16_test.tcl
```

串口输出：

```text
PS-PL-DDR GEMM sanity start
shape N=16 K=16 M=16
IP base    = 0x40000000
instr addr = 0x01010000
A addr     = 0x01020000
B addr     = 0x01030000
C addr     = 0x01040000
AP_CTRL before start = 0x00000004
AP_CTRL after wait = 0x00000006
done=1 idle=1 ready=0
ap_return = 1
checksum = 199688
mismatch_count = 0
PS-PL-DDR GEMM sanity PASS
```

这说明它不是只在 4x4 最小矩阵上碰巧通过。

## 放大到 112x112x112

最后我把 test shape 改成当前 HLS IP 的目标尺寸：

```text
N = 112
K = 112
M = 112
```

这一版 IP 的 HLS 脚本使用：

```text
GZY_ACCEL_BLOCK_N = 112
GZY_ACCEL_BLOCK_K = 112
GZY_ACCEL_BLOCK_M = 112
GZY_GEMM_TILE     = 14
```

所以 112x112x112 是当前板级 IP 的完整 block 尺寸。PS 端 timeout 也从 `10000000` 放大到 `100000000`，避免大矩阵还没算完就被误判 timeout。

运行脚本：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct_run_gemm112_test.tcl
```

串口输出：

```text
PS-PL-DDR GEMM sanity start
shape N=112 K=112 M=112
IP base    = 0x40000000
instr addr = 0x01010000
A addr     = 0x01020000
B addr     = 0x01030000
C addr     = 0x01040000
AP_CTRL before start = 0x00000004
AP_CTRL after wait = 0x00000006
done=1 idle=1 ready=0
ap_return = 1
checksum = -37432528
mismatch_count = 0
PS-PL-DDR GEMM sanity PASS
```

这里 `checksum` 是按 `int` 打印的校验和，显示为负数不是错误。真正的正确性判断是：

```text
mismatch_count = 0
```

112x112 一共有 12544 个输出元素，全部和 PS 端 CPU golden 对上。这个结果可以确认 PL 端确实通过 DDR 读写并完成了当前完整 block 尺寸的 GEMM。

## 这次真正证明了什么

这次不只是证明 PS 能写 AXI-Lite 寄存器，而是证明：

```text
PS 能把 A/B/instruction 放到 DDR
PS 能通过 AXI-Lite 启动 accelerator_top_axi
PL 能通过 m_axi/HP 口从 DDR 读 A/B/instruction
PL 能执行 GEMM
PL 能把 C 写回 DDR
PS 能通过 cache invalidate 读回正确 C
```

所以当前 Phase 3 的板级 GEMM 路径已经从 4x4 sanity 放大到了 112x112x112，并且结果正确。

## 当前还没有证明什么

这一版还不能说明大规模 GEMM 性能已经达标。它还没有验证：

```text
连续多条 GEMM 指令
不同 base offset 的多 buffer 调度
性能计时
长时间稳定性
更复杂的 Conv / Transformer instruction stream
```

下一步应该从单条 112 GEMM 继续往多指令、多 buffer 和计时方向走，而不是一口气跳到完整 Transformer。

## 后续计划

更稳的后续顺序：

```text
1. 重复跑 112x112x112 GEMM 多次，确认稳定性。
2. 加上简单 cycle 计时。
3. 测多条 GEMM instruction stream，例如 GEMM + GEMM + END。
4. 测不同 base offset 的 A/B/C buffer，确认多 buffer 调度。
5. 最后再把 Conv/Transformer 的数据映射进 instruction stream。
```

## 这一版结论

第 3 层已经跑通。现在可以说：

```text
Zynq PS 可以控制 HLS accelerator_top_axi。
HLS IP 可以通过 PL 侧 AXI master/HP 口访问 DDR。
4x4、16x16、112x112x112 GEMM 在板上计算结果正确。
```

这一步是 Phase 3 从“软件能启动板子”走向“PL 真的参与计算”的关键节点。
