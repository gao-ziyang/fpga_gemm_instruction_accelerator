# fpga_gemm_instruction_accelerator

这是我围绕 FPGA GEMM、CNN 卷积映射、Transformer Attention 和后续指令式加速器控制做的 HLS 学习工程。

我的阶段性路线是：

```text
INT8 GEMM 微核
  -> CNN Conv2D lowering/im2col 到 GEMM
  -> Transformer QKV / QK^T / P*V
  -> 最小 micro-instruction + accelerator_top
  -> 后续再考虑 PS-PL、DDR、AXI、串口调试和上板
```

## 当前状态

已经完成并验证：

1. `INT8 x INT8 -> INT32 accumulate` 的 tiled GEMM，GEMM core 不再硬编码输出右移。
2. `GEMM_MAX_N/K/M = 16/96/96`，`GEMM_TILE = 4` 的第一版扩容。
3. 固定参数 CNN Conv2D：`Input[3,6,6]`、`Weight[4,3,3,3]`，映射为 `A[16,27] x B[27,4]`。
4. Transformer QKV projection：`X[16,96] x W[96,96]`。
5. Attention score：`Q[16,96] x K^T[96,16]`。
6. Attention no-softmax：`Score_q[16,16] x V_q[16,96]`。
7. Attention row-normalization 近似版：`P_q[16,16] x V_q[16,96]`。
8. GEMM / Conv / QKV / Attention 均完成 C-sim、C-synth、C/RTL cosim。
9. 固定 `N=16,K=96,M=96` 的 GEMM 并行规模 sweep：`TILE=4/8/12/14`，用于比较 DSP 使用、RTL latency 和理论性能 gap。

这里的 `*_top()` 都是 HLS 单元验证入口；以后真正给 `accelerator_top()` 调用的应该是 `gemm_tiled()`、`conv2d_gemm()`、`qkv_projection()`、`attention_core()` 这类 core 函数。

## 当前核心参数

| 参数 | 当前值 | 我的理解 |
| --- | --- | --- |
| `GEMM_MAX_N` | 16 | HLS 综合时数组 N 维最大容量 |
| `GEMM_MAX_K` | 96 | HLS 综合时数组 K 维最大容量 |
| `GEMM_MAX_M` | 96 | HLS 综合时数组 M 维最大容量 |
| `GEMM_TILE` | 默认 4 | 局部计算 tile，也就是当前微核并行粒度；benchmark Tcl 可以用宏临时覆盖 |
| `GEMM_BLOCK_M` | 8 | B/C 按输出列分块缓存，降低一次缓存整列 M 的压力 |
| `gemm_data_t` | `ap_int<8>` | 输入和权重量化数据 |
| `gemm_acc_t` | `ap_int<32>` | 输出和中间累加数据 |

`GEMM_MAX_*` 是硬件综合出来的最大数组容量；`N/K/M` 是每次调用时传入的实际尺寸。比如当前 `gemm_tiled()` 可以用同一份硬件跑 `7x6x5` 的 GEMM，也可以支撑 `16x96x96` 级别的 QKV。

默认功能验证仍使用 `GEMM_TILE=4`，因为它对应 16 路局部 MAC，时序压力小，适合先验证 CNN/Transformer 映射关系。后续性能 sweep 会通过 Tcl 宏临时改成 `TILE=8/12/14`，用于观察更大并行阵列下的 DSP 使用、latency 和理论性能 gap。

当前我把 GEMM core 和量化后处理分开理解：

```text
gemm_tiled()
  -> 只做原始矩阵乘和 INT32 累加
  -> C = A x B

saturate_to_int8(x, shift)
  -> 需要继续喂给 INT8 GEMM 时再做
  -> 右移 shift + 饱和到 [-128,127]
```

这样做的好处是 GEMM 单元更干净，Python/C++ baseline 可以直接按矩阵乘对齐；CNN 输出如果只是作为 INT32 特征结果，不需要被固定右移；Transformer 的 Q/K/V、Score 这类中间值需要继续参与 INT8 GEMM 时，再由每个阶段单独配置 `q_shift`、`score_shift` 或 `p_shift`。

## 工程目录

```text
gzy_gemm_accel/
  README.md
  README_conv.md
  README_attention.md
  hls/
    src/
      gemm_types.h
      gemm_core.h
      gemm_core.cpp
      gemm_top.cpp
      gemm_bench_top.h
      gemm_bench_top.cpp
      conv_types.h
      conv_core.h
      conv_core.cpp
      conv_top.h
      conv_top.cpp
      qkv_projection.h
      qkv_projection.cpp
      qkv_top.cpp
      attention_core.h
      attention_core.cpp
      attention_top.cpp
    tb/
      tb_gemm.cpp
      tb_gemm_bench.cpp
      tb_conv.cpp
      tb_qkv.cpp
      tb_attention.cpp
    scripts/
      run_hls_gemm.tcl
      run_hls_gemm_benchmark_sweep.tcl
      run_hls_conv.tcl
      run_hls_qkv.tcl
      run_hls_attention_score.tcl
      run_hls_attention_no_softmax.tcl
      run_attention_hls.tcl
  python/golden/
  docs/
    iteration_001_minimal_gemm.md
    iteration_002_tiled_gemm.md
    iteration_003_buffer_boundary_quant.md
    iteration_004_qkv_projection.md
    iteration_005_conv2d_gemm.md
    iteration_006_attention.md
    iteration_007_expand_16_96_96.md
    iteration_008_conv_init_optimization.md
    iteration_009_gemm_benchmark_sweep.md
  vitis_hls_project/   # Vitis HLS 生成目录，本地保留，不上传
```

## 主要模块

| 模块 | 作用 | 数学含义 |
| --- | --- | --- |
| `gemm_tiled` | 通用 INT8 GEMM 核 | `C = A x B`，输出为 INT32 累加结果 |
| `conv2d_gemm` | Conv2D lowering/im2col 后调用 GEMM | `Conv2D -> A x B -> output` |
| `qkv_projection` | 复用同一个 X，依次计算 Q/K/V | `Q=XWq, K=XWk, V=XWv` |
| `attention_score_core` | 计算 Attention score | `Score = Q_q x K_q^T` |
| `attention_no_softmax_core` | 暂不做 softmax，先验证完整矩阵流 | `Out = Score_q x V_q` |
| `attention_core` | row-normalization 近似 attention | `Out = P_q x V_q` |

`Q/K/V` 先由 GEMM 得到 `gemm_acc_t`，后续再喂给 GEMM 前必须用 `saturate_to_int8()` 做右移、截断和饱和，转回 `gemm_data_t`。

## 如何运行

Python baseline：

```bash
python3 python/golden/gemm_4x4_baseline.py
python3 python/golden/qkv_projection_baseline.py
```

Windows / Vitis HLS 2020.2：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_conv.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_qkv.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_score.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_no_softmax.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_attention_hls.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm_benchmark_sweep.tcl
```

脚本都会依次执行：

```text
csim_design
csynth_design
cosim_design -rtl verilog
```

## 验证结果

| Case | 维度 | C-sim | C-synth | C/RTL cosim | mismatch | max_abs_error | checksum |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GEMM | `A[7,6] x B[6,5]` | PASS | PASS | PASS | 0 | 0 | 56726 |
| Conv2D via GEMM | `Input[3,6,6]`, `Weight[4,3,3,3]`, `A[16,27] x B[27,4]` | PASS | PASS | PASS | 0 | 0 | -72952 |
| QKV projection | `X[16,96] x W[96,96]` | PASS | PASS | PASS | 0 | 0 | 265116672 |
| Attention score | `Q[16,96] x K^T[96,16]` | PASS | PASS | PASS | 0 | 0 | -31663104 |
| Attention no-softmax + row-normalization | `Score_q[16,16] x V_q[16,96]` / `P_q[16,16] x V_q[16,96]` | PASS | PASS | PASS | 0 | 0 | 74785584 |
| GEMM benchmark sweep | `A[16,96] x B[96,96]`, `TILE=4/8/12/14` | PASS | PASS | PASS | 0 | 0 | 101159936 |

## 综合与 cosim 摘要

| Top | BRAM_18K | DSP | FF | LUT | Estimated clock | RTL latency |
| --- | --- | --- | --- | --- | --- | --- |
| `gemm_top` | 2 | 16 | 3914 | 5226 | 7.142 ns | 544 cycles |
| `conv_top` | 15 | 16 | 1894 | 3574 | 7.103 ns | 2594 cycles |
| `qkv_top` | 2 | 16 | 3921 | 5375 | 7.300 ns | 360362 cycles |
| `attention_score_top` | 10 | 17 | 4517 | 5764 | 7.050 ns | max 23025 cycles |
| `attention_no_softmax_top` | 37 | 49 | 13135 | 18117 | 7.300 ns | max 407139 cycles |
| `attention_top` | 37 | 49 | 15884 | 20155 | 7.300 ns | max 408064 cycles |

`attention_top` 里 row-normalization 目前使用整数除法，HLS 生成了 `sdiv`，所以它是一个能跑通的第一版近似，不是最终资源优化版本。

## GEMM 并行规模 sweep

这一组实验固定 `N=16,K=96,M=96`，也就是 `total MAC = 147456`，只改变 `GEMM_TILE` 和对应的 `GEMM_BLOCK_M`。实际吞吐使用 C/RTL cosim 的 Verilog latency 计算：

```text
actual_mac_per_cycle = total_mac / rtl_latency
GMAC/s @100MHz = actual_mac_per_cycle * 0.1
GOPS @100MHz = GMAC/s * 2
```

| TILE | BLOCK_M | DSP | BRAM_18K | FF | LUT | Estimated clock | RTL latency | Actual MAC/cycle | GMAC/s @100MHz | GOPS @100MHz |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 4 | 8 | 16 | 2 | 1599 | 1552 | 7.136 ns | 118720 | 1.242 | 0.124 | 0.248 |
| 8 | 8 | 64 | 2 | 5448 | 3441 | 7.136 ns | 54784 | 2.692 | 0.269 | 0.538 |
| 12 | 12 | 144 | 2 | 11865 | 6824 | 7.136 ns | 52972 | 2.784 | 0.278 | 0.557 |
| 14 | 14 | 197 | 2 | 16271 | 9393 | 7.136 ns | 54492 | 2.706 | 0.271 | 0.541 |

这组结果说明 DSP 使用量确实能随 `TILE*TILE` 增大，但实际吞吐没有线性提升。主要原因是当前 latency 统计的是完整 `gemm_tiled()` 调用，包括 A/B 搬入片上缓存、local tile 搬运、边界处理和写回；这些阶段还没有和 `dot_k` 计算阶段做 dataflow overlap。`TILE=14` 已经使用 197 个 DSP，接近 ZYNQ-7020 的 DSP 上限，适合作为“接近吃满 DSP”的性能对比点，但后续系统集成时仍需要给 Conv/Attention 外围逻辑预留资源。

Conv2D 这里已经做了两步优化：先去掉 wrapper 里对 `A/B/C` 最大矩阵的全量初始化，再把 `conv_top()` 外部接口改成 flat `input[108]`、`weight[108]`、`output[64]`，并用自增地址写 im2col/weight flatten/reshape。这样避免 HLS 为多维数组和非 2 的幂循环拍平生成大量 `urem/div/mul` 地址逻辑，Conv RTL latency 从 `15448 cycles` 降到 `2594 cycles`，额外 DSP 也从 36 降回 GEMM 微核本身的 16。

## 环境

| 项目 | 当前值 |
| --- | --- |
| OS | WSL2 Ubuntu on Windows |
| Vitis HLS / Vivado | 2020.2 |
| Target FPGA | `xc7z020clg400-2` |
| Target clock | 10 ns |
| C++ compiler | Vitis HLS 自带 GCC |
| Python | 3.12.3 |
| 当前未使用 | PyTorch、Verilator、cocotb、上板 Vivado block design |

## 后续路径思考

我一开始会把任务 2 想得像“做一个 CPU 或 RISC-V”，现在更准确的理解是：先做一个面向神经网络算子的 micro-instruction 控制器。也就是固定宽度指令字里放 `opcode`、shape、scale、buffer id 等字段，然后 `accelerator_top()` 负责取指、译码、配置参数和调用 `gemm_tiled/conv2d_gemm/qkv_projection/attention_core`。

后续上板时，我也想过能不能 PC 直接串口连 PL 顶层。现在我的理解是：不太建议这样做。串口通常先进 PS 或外部 USB-UART 控制逻辑，再由 PS 通过 AXI-Lite/AXI Master/DMA 去驱动 PL 加速器。PL 更适合做确定的数据通路和计算阵列；PS 更适合做串口协议、DDR 管理、启动停止和状态读取。

所以我后面的优先级是：

```text
1. 保持各算子 core 稳定。
2. 做 C-sim 友好的 accelerator_top 指令解释器。
3. 再把最终 accelerator_top 的接口改成 AXI-Lite 控制 + DDR/AXI/DMA 数据通路。
4. 最后再考虑串口作为 PC 与 PS 的调试入口。
```
