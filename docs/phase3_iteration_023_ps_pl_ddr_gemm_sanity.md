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
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm_test.tcl
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
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm16_test.tcl
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
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm112_test.tcl
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

## 补充验证：112 计时、重复运行、多指令和多 buffer

112x112x112 单条 GEMM 通过以后，我没有马上跳到 1024/2048，而是先补了几个更像“系统 sanity”的小测试：

```text
1. 给 112x112x112 加 XTime 计时。
2. 在同一个 PS 程序里重复跑 112 单 GEMM 3 次。
3. 用 XSCT repeat 脚本连续下载/运行 ELF 3 次。
4. 加 GEMM + GEMM + END，验证多指令流。
5. 给第二条 GEMM 使用不同的 a_base/b_base/c_base，验证多 buffer offset。
```

对应的 PS 源码快照是：

```text
ps_apps/accel_axi_112_gemm_test/helloworld.c
```

对应的 XSCT 脚本现在放到：

```text
scripts/xsct/xsct_run_gemm112_test.tcl
scripts/xsct/xsct_repeat_gemm112_test.tcl
```

我把 XSCT 脚本单独放在 `scripts/xsct/`，和 `hls/scripts/` 区分开：

```text
hls/scripts/      Vitis HLS 综合、导出 IP 用
scripts/xsct/     XSCT 上板下载 ELF、运行、重复测试用
```

这次串口里，112 单 GEMM 在程序内重复 3 次：

```text
--- 112 single GEMM run 1/3 ---
timer_ticks = 1154998
approx_cpu_cycles = 2309996
time_us = 3464
ap_return = 1
single112 checksum32 = 0x6F959B25
single112 mismatch_count = 0

--- 112 single GEMM run 2/3 ---
timer_ticks = 1154879
approx_cpu_cycles = 2309758
time_us = 3464
ap_return = 1
single112 checksum32 = 0x54DBE265
single112 mismatch_count = 0

--- 112 single GEMM run 3/3 ---
timer_ticks = 1154892
approx_cpu_cycles = 2309784
time_us = 3464
ap_return = 1
single112 checksum32 = 0x39605B25
single112 mismatch_count = 0
```

后面用 `xsct_repeat_gemm112_test.tcl` 连续下载运行 ELF 3 轮，每轮程序内部又跑 3 次 112，所以这一组里实际看到了 9 次 112 单 GEMM 通过。不同 run 的 `time_us` 大致在：

```text
3463 ~ 3466 us
```

这说明当前 112 单 GEMM 的板级运行时间比较稳定。

多指令测试用的是：

```text
GEMM + GEMM + END
```

其中两条 GEMM 都先用 56x56x56，而不是两条 112x112x112。原因是当前这个 bitstream 的 `GZY_ACCEL_MAX_N/K/M` 和 m_axi depth 仍是 112。如果在同一个 A/B/C memory 里塞两块完整 112 buffer，会超过当前 IP 声明的可访问元素范围。56x56 的两块 buffer 可以安全放下，所以更适合用来验证“多指令 + 多 buffer offset”。

本次使用的 offset 是：

```text
A1_BASE_ELEMS = 4096
B1_BASE_ELEMS = 4096
C1_BASE_ELEMS = 4096
```

这里的 base 是 HLS 侧数组元素偏移，不是统一的字节地址。A/B 是 int8，所以 4096 elements 也是 4096 bytes；C 是 int32，所以 4096 elements 是 16 KB。

串口结果：

```text
--- multi instruction and multi buffer test ---
run instruction stream: GEMM + GEMM + END
AP_CTRL before start = 0x00000004
AP_CTRL after wait = 0x00000006
done=1 idle=1 ready=0
timer_ticks = 1273415
approx_cpu_cycles = 2546830
time_us = 3820
ap_return = 2

multi56_c0 checksum32 = 0x926D5245
multi56_c0 mismatch_count = 0
multi56_c1 checksum32 = 0xC5B7D165
multi56_c1 mismatch_count = 0

PS-PL-DDR GEMM extended sanity PASS
```

这里 `ap_return = 2` 很重要。它说明 `execute_instruction_stream` 在 PL 里确实执行了两条 GEMM 指令，不是 PS 端单纯打印了两次 PASS。`multi56_c0` 和 `multi56_c1` 都对齐 CPU golden，说明第二条 GEMM 写到了另一块 C buffer，而且 PS 端能读回正确结果。

## 这次真正证明了什么

这次不只是证明 PS 能写 AXI-Lite 寄存器，而是证明：

```text
PS 能把 A/B/instruction 放到 DDR
PS 能通过 AXI-Lite 启动 accelerator_top_axi
PL 能通过 m_axi/HP 口从 DDR 读 A/B/instruction
PL 能执行 GEMM
PL 能把 C 写回 DDR
PS 能通过 cache invalidate 读回正确 C
instruction stream 可以连续执行 GEMM + GEMM + END
base offset 可以切到不同 DDR buffer
```

所以当前 Phase 3 的板级 GEMM 路径已经从 4x4 sanity 放大到了 112x112x112，并且结果正确。

## 当前还没有证明什么

这一版还不能说明大规模 GEMM 性能已经达标。它已经验证了 112 计时、重复运行、多指令和多 buffer，但还没有验证：

```text
大尺寸 MAX/depth 下的 1008/1024 GEMM
更长时间的老化稳定性
更复杂的 Conv / Transformer instruction stream
完整 Conv / Attention 在板上的结果对齐
```

下一步应该先回到 PL/HLS，把当前只验证到 112 的 AXI IP 做成大尺寸兼容版本，再迁移 Conv/Attention 到当前 `gemm_scheduler + instruction stream` 体系里。

## 后续计划

我现在更认同的后续顺序：

```text
1. 先把 PL 端 AXI IP 的 MAX/depth 从 112 放大到一个明确目标，例如 1008。
2. 用新的大尺寸 IP 先跑 1008x1008x1008，验证 full-block 主路径。
3. 再跑 1024x1024x1024，验证边界 block。
4. 把已有 Conv 和 Attention 迁移到当前 gemm_scheduler 版本。
5. 扩展 opcode/decode/dispatch，形成 GEMM / Conv / Attention 的最小指令集。
6. PS 端分别做 GEMM、Conv、Attention 三类板级 golden check。
7. 三类功能闭环都稳定后，再集中优化 GEMM 资源和 latency。
```

这个顺序的好处是先把功能边界固定住，再做性能优化。否则一边改大矩阵、一边加 Conv/Attention、一边优化资源，失败时很难判断是哪一层引入的问题。

## 这一版结论

第 3 层已经跑通。现在可以说：

```text
Zynq PS 可以控制 HLS accelerator_top_axi。
HLS IP 可以通过 PL 侧 AXI master/HP 口访问 DDR。
4x4、16x16、112x112x112 GEMM 在板上计算结果正确。
112 单 GEMM 的计时和重复运行稳定。
GEMM + GEMM + END 多指令流已经通过。
不同 DDR buffer offset 已经通过。
```

这一步是 Phase 3 从“软件能启动板子”走向“PL 真的参与计算”的关键节点。
