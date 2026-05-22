# Iteration 010：先不管 AXI，把 GEMM core、scheduler、指令解码和 accelerator_top 跑通

## 我这一版想解决什么

这次我先不做 AXI、DDR、PS 端软件和 Vivado block design，而是先在 HLS 里把后面真正需要的几层结构搭起来：

```text
gemm_core_mac()
  -> gemm_scheduler()
  -> execute_instruction_stream() / decode_instruction()
  -> accelerator_top()
```

我现在对这个任务的理解是：先把“计算阵列”和“调度控制”拆开。`gemm_core_mac()` 只负责一个局部 tile 的 MAC 阵列；`gemm_scheduler()` 负责把大矩阵手动分块、搬到片上 buffer、调用计算阵列并管理 C 的 partial sum；指令解码层再把一条指令里的 `opcode/N/K/M/base` 等字段解释出来，决定调用哪个调度模块。

所以这一版分成三个验证入口：

| 版本 | HLS top | 这一版验证什么 |
| --- | --- | --- |
| V1 | `gemm_scheduler_top` | 不经过指令，直接验证大矩阵分块调度和 GEMM core |
| V2 | `instruction_decode_top` | 增加 128-bit 指令字、decode 和 dispatch |
| V3 | `accelerator_top` | 最终非 AXI 版顶层，先用 `ap_memory` 做单元验证 |

这里的 `*_top` 仍然是 HLS 验证入口，不是最终上板接口。后面真正上板时，`accelerator_top` 的端口还需要重新规划成 AXI-Lite 控制和 AXI Master/DMA 数据通路。

## 我改了哪些地方

### 1. 把 GEMM core 更明确地独立出来

我在 `gemm_core.cpp` 里新增了：

```cpp
void gemm_core_mac(
    gemm_data_t localA[GEMM_TILE][GEMM_TILE],
    gemm_data_t localB[GEMM_TILE][GEMM_TILE],
    gemm_acc_t localC[GEMM_TILE][GEMM_TILE]
);
```

这个函数只做：

```text
localC += localA x localB
```

里面 `localA/localB/localC` 都是完全 partition，`ii/jj` 展开，`kk` 做 pipeline。这样我后面解释时可以把它当成真正的 MAC 阵列：

```text
TILE = 12
并行 MAC 规模约等于 12 x 12 = 144
主要消耗 DSP
```

原来的 `gemm_tiled()` 仍然保留，因为 Conv/QKV/Attention 的单元验证还会用它；只是它内部的 `dot_k` 阶段也改成调用 `gemm_core_mac()`，这样两条路线都复用同一个最底层计算核。

### 2. 新增 gemm_scheduler()

新增文件：

```text
hls/src/gemm_scheduler.h
hls/src/gemm_scheduler.cpp
hls/src/gemm_scheduler_top.cpp
hls/tb/tb_gemm_scheduler.cpp
```

这一层的职责是手动完成大 GEMM 的分块和映射：

```text
外部 A_mem/B_mem/C_mem
  -> A_buf/B_buf/C_buf 片上 block buffer
  -> localA/localB/localC
  -> gemm_core_mac()
  -> C_buf partial sum
  -> C_mem
```

这次先用：

```text
N = 1024
K = 1024
M = 1024
TILE = 12
BLOCK_N = 96
BLOCK_K = 96
BLOCK_M = 96
```

`BLOCK_N/K/M = TILE * 8`。我这么选的原因是：`TILE=12` 已经能用到 144 个 DSP，先不一上来用 `TILE=14` 把资源压得太满；block 取 96 可以让片上 buffer 有比较明显的数据复用，同时总 buffer 规模还在 ZYNQ-7020 可以接受的范围内。

### 3. 新增 instruction/decode

新增文件：

```text
hls/src/accelerator_types.h
hls/src/accelerator_instruction.h
hls/src/accelerator_instruction.cpp
hls/src/instruction_decode_top.cpp
hls/tb/tb_instruction_decode.cpp
```

我先用了 128-bit 指令字，字段大致是：

```text
[7:0]     opcode
[23:8]    N
[39:24]   K
[55:40]   M
[79:56]   A base
[103:80]  B base
[127:104] C base
```

当前只定义了：

```text
ACCEL_OP_GEMM = 1
ACCEL_OP_END  = 0
```

这一步还不是完整 ISA，只是先证明“指令描述一个 GEMM 层，然后硬件译码并调用调度模块”这个流程能跑通。

### 4. 新增 accelerator_top()

新增文件：

```text
hls/src/accelerator_top.h
hls/src/accelerator_top.cpp
hls/tb/tb_accelerator_top.cpp
```

现在的顶层接口还是：

```text
instr_mem: ap_memory
A_mem/B_mem/C_mem: ap_memory
instr_num: ap_none
status: ap_memory / ap_vld
return: ap_ctrl_hs
```

这不是最终上板接口，只是为了 C-sim、C-synth 和 C/RTL cosim 先跑通控制流。

## 我学到的东西

这一版我主要补了几件事：

1. `gemm_core_mac()` 和 `gemm_scheduler()` 要分层。前者是计算并行度，后者才是片上缓存容量和数据映射。
2. `BIND_STORAGE type=ram_2p impl=bram` 可以明确告诉 HLS 把 block buffer 放到双端口 BRAM。
3. `ARRAY_PARTITION cyclic factor=GEMM_TILE dim=2` 是为了给 local tile 加载提供更多 bank，不然大 TILE 的 local load 更容易被端口卡住。
4. `ap_uint<128>` 可以模拟固定宽度指令字，后续再换成 AXI/DDR 里的指令流。
5. HLS top 如果参数是运行时指令字段，综合报告的最坏 latency 可能不可信，需要结合固定尺寸 top 或 RTL cosim 结果解释。

## 一次 RTL cosim 暴露的问题

一开始 V1 的 C-sim 是 PASS 的，但 128 规模 RTL cosim 失败，mismatch 从第二个 M block 开始出现，比如 `C[0][96]` 附近不对。

我定位后发现问题在 `C_buf` 的 partial sum 管理。原先我在每个 `(n0,m0)` block 开始时清一次整个 `C_buf`，然后每个 `k0` 累加。但 RTL cosim 里第二个 M block 出现了旧值残留。这个问题 C-sim 没暴露出来，说明 C-sim 不能替代 RTL cosim。

后面我把逻辑改成：

```text
每个 localC tile 在 k0 == 0 时清零；
如果 k0 != 0，就从 C_buf 读出上一个 K block 的 partial sum；
完成当前 K block 后再写回 C_buf。
```

也就是新增了 `reset_c = (k0 == 0)`，并删除了整块 `C_buf` 初始化。这更符合 GEMM partial sum 的语义，也减少了一次大范围清零。修改后 V1/V2/V3 的 128 规模 C/RTL cosim 都通过了。

## 验证命令

一键跑全部脚本：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_v123.tcl
```

分开跑：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_v1_scheduler.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_v2_decode.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_v3_top.tcl
```

128 规模 RTL cosim：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_v1_scheduler_cosim_small.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_v2_decode_cosim_small.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_v3_top_cosim_small.tcl
```

1024 规模的 RTL cosim 会跑数千万周期，日常迭代太慢，所以这次是：

```text
1024^3：C-sim + C-synth
128^3：C-sim + C-synth + C/RTL cosim
```

## 1024 规模验证结果

测试规模：

```text
N = 1024
K = 1024
M = 1024
total MAC = 1073741824
TILE = 12
BLOCK_N/K/M = 96
```

C-sim 输出摘要：

```text
[V1] N=1024 K=1024 M=1024 TILE=12 BLOCK_N=96 BLOCK_K=96 BLOCK_M=96 total_mac=1073741824
[V1] mismatch_count=0
[V1] checksum=2087749971
[V1] PASS

[V2] N=1024 K=1024 M=1024 TILE=12 BLOCK_N=96 BLOCK_K=96 BLOCK_M=96 total_mac=1073741824
[V2] mismatch_count=0
[V2] checksum=2087749971
[V2] PASS

[V3] N=1024 K=1024 M=1024 TILE=12 BLOCK_N=96 BLOCK_K=96 BLOCK_M=96 total_mac=1073741824
[V3] mismatch_count=0
[V3] checksum=2087749971
[V3] PASS
```

综合结果摘要：

| 版本 | Top | C-sim | C-synth | BRAM_18K | DSP | FF | LUT | Estimated clock | Latency 说明 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| V1 | `gemm_scheduler_top` | PASS | PASS | 48 | 144 | 22842 | 15967 | 7.111 ns | `47393183 cycles` |
| V2 | `instruction_decode_top` | PASS | PASS | 48 | 147 | 24629 | 17599 | 7.653 ns | top 最坏值不可信，见下文 |
| V3 | `accelerator_top` | PASS | PASS | 48 | 147 | 24629 | 17612 | 7.653 ns | top 最坏值不可信，见下文 |

V2/V3 的 top 综合报告里 latency 最大值非常离谱，这是因为指令里的 `N/K/M` 是运行时字段，HLS 不知道我 testbench 实际只跑一条 `1024x1024x1024` GEMM 指令，于是按动态循环给了一个很保守的最坏情况。性能分析时我更应该看 V1 固定尺寸 scheduler 的报告；V2/V3 主要证明取指、译码和 dispatch 逻辑正确。

V1 的实际吞吐按综合 latency 估算：

```text
actual MAC/cycle = 1073741824 / 47393183 = 22.656
```

理想情况下，`TILE=12` 的 MAC 阵列每周期最多可以做：

```text
12 x 12 = 144 MAC/cycle
```

所以现在的 gap 还是很明显。这个 gap 不是“乘法器没展开”，因为 DSP 已经用了 144 个；更主要是 block load、local load、store 和循环控制还没有和计算阶段重叠。

## 128 规模 RTL cosim 结果

128 规模主要是为了让 RTL cosim 能在可接受时间内跑完，同时仍然覆盖 `BLOCK=96` 后的边界 block。

| 版本 | Top | C-sim | C-synth | C/RTL cosim | Verilog latency | checksum |
| --- | --- | --- | --- | --- | --- | --- |
| V1 | `gemm_scheduler_top` | PASS | PASS | PASS | 315074 | 35200361 |
| V2 | `instruction_decode_top` | PASS | PASS | PASS | 315251 | 35200361 |
| V3 | `accelerator_top` | PASS | PASS | PASS | 315251 | 35200361 |

V2/V3 比 V1 多了大约 177 cycles，主要来自取指、译码、状态返回这些控制路径。这个开销相对 GEMM 主体不大。

## 这一版的问题和后续想法

现在还没有做 DATAFLOW，也没有做 double buffer，所以数据流还是比较阶段式：

```text
load_a_block
load_b_block
compute_block
store_c_block
```

在 `compute_block` 内部也还是：

```text
load_local_c
for each K tile:
  load_local_a
  load_local_b
  gemm_core_mac
store_local_c
```

所以即使 `gemm_core_mac()` 内部有 144 个 MAC，整体吞吐也会被 local load、BRAM bank、边界判断、C partial sum 读写拖住。

我下一步的优化路线应该是：

1. 先把 `TILE=14` 版本跑通，看 196 路 MAC 时资源和时序是否还能接受。
2. 尝试让 `load_local_a` 和 `load_local_b` 并行，看看能不能减少每个 K tile 的加载等待。
3. 再考虑 double buffer，让下一组 localA/localB 的加载和当前计算重叠。
4. 最后再动更激进的 BRAM banking，因为 banking 会增加 BRAM bank、mux 和布线压力。

目前我先保留 Conv/Attention 的单元验证文件，因为它们是前面任务 1 的功能交付；这次新加的 V1/V2/V3 是更接近后续系统化加速器的验证路径。
