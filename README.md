# fpga_gemm_instruction_accelerator

这是一个基于 Vitis HLS 的小型 FPGA GEMM / 算子调度学习工程。当前目标不是一开始做完整模型，而是先把老师任务里的关键计算逐步拆开：

```text
INT8 GEMM 微核
  -> CNN 卷积层映射
  -> Transformer Attention / Encoder 子模块
  -> 64-bit micro-instruction 控制的 accelerator_top
  -> 后续 PS-PL / DDR / AXI 上板接口
```

当前工程已经实现并验证了一个小型 `INT8 x INT8 -> INT32 -> >> 8` 的 tiled GEMM 核，并用它完成了 Q/K/V projection 的第一版验证。后续会继续补 CNN 和 Transformer 的小模块，最后再进入任务 2 的指令化控制器。

## 当前状态

已经完成：

1. 最小 `4x4` INT8 GEMM。
2. 扩展到运行时 `N/K/M` 的 tiled GEMM。
3. 加入 `A_bram`、`B_bram`、`localA/localB/localC`，理解片上 buffer 和局部并行访问。
4. 支持 `N/K/M` 不是 tile 整数倍时的边界处理。
5. 支持 `INT8 x INT8 -> INT32 accumulate -> >> 8` 的简单量化右移。
6. 加入 `update_A`，在连续 GEMM 中复用 `A_bram`。
7. 用当前 GEMM 实现 QKV projection：`Q=XWq`、`K=XWk`、`V=XWv`。
8. 使用 Python baseline、HLS C simulation、HLS C synthesis 做验证。

当前最新 QKV 验证：

```text
X[7,6] x Wq/Wk/Wv[6,6] -> Q/K/V[7,6]
Q/K/V = (X x W*) >> 8
mismatch_count = 0
max_abs_error  = 0
checksum       = 93935
```

## 后续任务目标

### 阶段 1：补齐 CNN 小模块

先把 CNN 卷积和 GEMM 的关系讲清楚，再做一个可验证的 HLS 小模块。

计划：

1. 先实现 `1x1 conv`，因为它可以直接看成 GEMM。
2. 再考虑 `3x3 conv + im2col`，把卷积窗口展开成矩阵。
3. 写 `conv_top()` 和 `tb_conv.cpp`。
4. 写 Python baseline，对比 HLS C-sim 输出。
5. 综合后记录 LUT/FF/DSP/BRAM、II、Fmax。

第一版只需要证明“一个卷积层可以映射到 GEMM 计算单元”，不急着做完整 CNN 网络。

### 阶段 2：补齐 Transformer 小模块

当前已经完成 QKV projection。后续继续往 Attention 方向推进。

计划：

1. 继续验证 `QK^T`：`S = Q x K^T`。
2. 继续验证 `S x V`：`O = S x V`。
3. 先做一个 attention 子模块，不急着一上来做多层多头完整 Encoder。
4. 如果时间允许，再整理成一个最小 `attention_top()` 或简化 `encoder_block_top()`。
5. 同样用 Python baseline + HLS C-sim + HLS C-synth 验证。

这一阶段重点是说明：Transformer Encoder 里的核心计算也可以拆成几类矩阵乘。

### 阶段 3：任务 2，最小指令集和 accelerator_top

老师说的第二个任务，我现在更准确地理解为：

```text
设计一套小型神经网络算子 micro-instruction，
用固定宽度指令描述网络层或算子，
再由 accelerator_top 取指、译码、配置参数、调用计算单元。
```

它不是完整 RISC-V，也不是通用 CPU，而是一个面向 GEMM / Conv / Attention 的最小算子指令控制器。

第一版建议使用 64-bit 指令字：

```cpp
typedef ap_uint<64> instr_word_t;
```

示例字段：

```text
[63:56] opcode
[55:48] flags / func
[47:40] dst buffer id
[39:32] src0 buffer id
[31:24] src1 buffer id
[23:16] arg0
[15:8]  arg1
[7:0]   arg2
```

最小 opcode 可以先定义：

```text
CONFIG_NKM
CONFIG_SCALE
GEMM
CONV2D
QKV
ATTN
END
```

第一版 `accelerator_top()` 可以先用 HLS 数组模拟 instruction memory 和 data buffer，重点验证：

```text
取指 -> 译码 -> 配置寄存器 -> 调用 GEMM/Conv/Transformer 子模块 -> 状态返回
```

等 C-sim / C-synth 跑通后，再考虑把顶层接口改成 AXI-Lite + AXI Master / DMA。

### 阶段 4：上板接口规划

小模块阶段的 `gemm_top()`、`conv_top()`、`qkv_top()` 主要用于单元验证。真正上板时，应该重点规划系统级顶层：

```text
accelerator_top()
```

可能的数据流：

```text
PS 把 instruction / A / B / weights 写入 DDR
PS 通过 AXI-Lite 写 start、instr_base、data_base、instr_num 等控制寄存器
PL accelerator_top 通过 AXI Master 或 DMA 读取 DDR
PL 执行指令并写回结果
PS 等待 done，然后读取输出
```

因此最终顶层接口更可能是：

```cpp
#pragma HLS INTERFACE m_axi     port=instr_mem offset=slave bundle=gmem0
#pragma HLS INTERFACE m_axi     port=A         offset=slave bundle=gmem1
#pragma HLS INTERFACE m_axi     port=B         offset=slave bundle=gmem2
#pragma HLS INTERFACE m_axi     port=C         offset=slave bundle=gmem3

#pragma HLS INTERFACE s_axilite port=instr_mem bundle=control
#pragma HLS INTERFACE s_axilite port=A         bundle=control
#pragma HLS INTERFACE s_axilite port=B         bundle=control
#pragma HLS INTERFACE s_axilite port=C         bundle=control
#pragma HLS INTERFACE s_axilite port=instr_num bundle=control
#pragma HLS INTERFACE s_axilite port=return    bundle=control
```

这一部分后续再做。目前先把算子和指令控制逻辑在 C-sim 里跑通。

## 工程目录

```text
gzy_gemm_accel/
  README.md
  .gitignore
  hls/
    src/
      gemm_types.h       # GEMM 尺寸、tile、量化右移和 ap_int 类型
      gemm_core.h        # gemm_tiled 与 gemm_top 的函数声明
      gemm_core.cpp      # tiled GEMM 核心实现
      gemm_top.cpp       # GEMM HLS 顶层函数和接口 pragma
      qkv_projection.h   # QKV projection 函数声明
      qkv_projection.cpp # 用 gemm_tiled 计算 Q/K/V
      qkv_top.cpp        # QKV HLS 顶层函数
    tb/
      tb_gemm.cpp        # GEMM C simulation testbench
      tb_qkv.cpp         # QKV C simulation testbench
    scripts/
      run_hls_gemm.tcl   # 自动运行 GEMM csim/csynth
      run_hls_qkv.tcl    # 自动运行 QKV csim/csynth
  python/
    golden/
      gemm_4x4_baseline.py
      qkv_projection_baseline.py
  docs/
    iteration_001_minimal_gemm.md
    iteration_002_tiled_gemm.md
    iteration_003_buffer_boundary_quant.md
    iteration_004_qkv_projection.md
  reports/               # 后续手动整理的报告摘要
  vitis_hls_project/     # Vitis HLS 工具生成目录，本地保留，不上传
```

源码主要维护在 `hls/src`、`hls/tb`、`hls/scripts` 和 `python/golden`。`vitis_hls_project/` 是工具生成目录，已经被 `.gitignore` 忽略。

## 当前功能文件说明

| 文件 | 作用 |
| --- | --- |
| `hls/src/gemm_types.h` | 定义 `GEMM_MAX_N/K/M`、`GEMM_TILE`、`GEMM_BLOCK_M`、`GEMM_OUT_SHIFT` 和数据类型。 |
| `hls/src/gemm_core.h` | 声明 `gemm_tiled` 和 HLS 顶层 `gemm_top`。 |
| `hls/src/gemm_core.cpp` | 实现 tiled GEMM、片上 buffer、边界补 0、右移输出和 `update_A`。 |
| `hls/src/gemm_top.cpp` | GEMM 单元验证用 HLS 顶层，设置 `ap_memory` 和 `ap_ctrl_hs`。 |
| `hls/src/qkv_projection.cpp` | 顺序调用三次 `gemm_tiled`，计算 Q/K/V。 |
| `hls/src/qkv_top.cpp` | QKV 单元验证用 HLS 顶层。 |
| `hls/tb/tb_gemm.cpp` | 生成固定 A/B，调用 `gemm_top`，用 C++ golden 检查输出。 |
| `hls/tb/tb_qkv.cpp` | 生成固定 X/Wq/Wk/Wv，检查 Q/K/V 输出。 |
| `python/golden/gemm_4x4_baseline.py` | Python GEMM baseline。 |
| `python/golden/qkv_projection_baseline.py` | Python QKV baseline。 |

## 环境说明

| 项目 | 当前值 |
| --- | --- |
| OS | WSL2 Ubuntu on Windows |
| Vitis HLS / Vivado version | Vitis HLS 2020.2；Vivado 2020.2 已安装但当前未使用 |
| C++ compiler | HLS C-sim 使用 Vitis HLS 自带 GCC |
| Python version | Python 3.12.3 |
| NumPy version | 当前 baseline 未使用 NumPy |
| Target FPGA | `xc7z020clg400-2` |
| Target clock | 10 ns，即 100 MHz |

当前没有使用 `uv`、PyTorch、cocotb、iverilog、Verilator 或 Vivado RTL 仿真。当前验证主要依赖 Python 和 Vitis HLS C simulation / C synthesis。

## 当前 GEMM 核心参数

| 参数 | 值 | 含义 |
| --- | --- | --- |
| `GEMM_MAX_N` | 8 | 当前 HLS 数组行维度综合上限 |
| `GEMM_MAX_K` | 8 | 当前 K 维综合上限 |
| `GEMM_MAX_M` | 8 | 当前输出列维度综合上限 |
| `GEMM_TILE` | 4 | 局部 `4x4` GEMM tile |
| `GEMM_BLOCK_M` | 8 | B/C 按输出列缓存的块宽；当前等于 `GEMM_MAX_M`，主要是为后续大 M 分块预留 |
| `GEMM_OUT_SHIFT` | 8 | 输出右移位数，模拟定点量化 scale |
| `gemm_data_t` | `ap_int<8>` | A/B 输入数据类型 |
| `gemm_acc_t` | `ap_int<32>` | C 输出和中间累加数据类型 |

`GEMM_MAX_*` 是综合时固定数组上限；`N/K/M` 是运行时传入的实际规模。当前最大尺寸设为 8，是为了快速学习、仿真和综合；后续会逐步扩展到更接近 Transformer 的规模。

## 当前硬件结构理解

当前 `gemm_tiled` 可以理解为：

```text
外部 A/B ap_memory
  -> update_A=true 时，A_bram 缓存 A 的有效区域
  -> update_A=false 时，A_bram 复用上一轮 A
  -> B_bram 缓存当前输出列块
  -> localA/localB/localC 完全 partition
  -> dot_k 中 16 路局部输出并行更新
  -> localC >> 8 写回 C 的有效区域
```

这里还不是完整 systolic array，也没有 AXI DMA，但已经包含 FPGA GEMM 微核最基础的几个点：tile 分块、K 维累加、片上数据复用、局部并行 MAC、边界补 0 和定点右移。

## 顶层接口理解

当前 `gemm_top()` 里写：

```cpp
#pragma HLS INTERFACE ap_memory port=A
#pragma HLS INTERFACE ap_memory port=B
#pragma HLS INTERFACE ap_memory port=C
#pragma HLS INTERFACE ap_ctrl_hs port=return
```

这是单元验证阶段比较简单的接口写法：

```text
A/B/C 是数组，所以先让 HLS 生成 memory-like 端口；
N/K/M/update_A 是标量，HLS 可以推断成普通输入端口；
ap_ctrl_hs 生成 start/done/idle/ready 这类函数级握手信号。
```

这些小模块 top 主要服务于 C-sim、C-synth 和之后可能的 C/RTL cosim。真正上板时，重点应该在最终 `accelerator_top()` 统一规划 AXI-Lite、AXI Master 或 DMA 接口。

## 如何运行

### 1. GEMM Python baseline

```bash
cd /mnt/c/Transformer/gzy_gemm_accel
python3 python/golden/gemm_4x4_baseline.py
```

### 2. QKV Python baseline

```bash
cd /mnt/c/Transformer/gzy_gemm_accel
python3 python/golden/qkv_projection_baseline.py
```

### 3. GEMM HLS C simulation and C synthesis

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
```

### 4. QKV HLS C simulation and C synthesis

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_qkv.tcl
```

## 验证结果

| Case | 维度 | Baseline | HLS C-sim | 结果 | max_abs_error | mismatch_count | checksum |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GEMM basic | `A[4,4] x B[4,4]` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | 见日志 001 |
| Tiled GEMM | `A[8,8] x B[8,8]` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | 358080 |
| Buffer + Boundary + Quant GEMM | `A[7,6] x B[6,5]`，`>> 8` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | -140 |
| QKV projection | `X[7,6] x Wq/Wk/Wv[6,6]`，`>> 8` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | 93935 |
| CNN Conv via GEMM | 先做 `1x1 conv`，再考虑 `3x3 im2col` | Python | HLS C-sim | 待完成 | 待完成 | 待完成 | 待完成 |
| Attention QK^T | `Q x K^T` | Python | HLS C-sim | 待完成 | 待完成 | 待完成 | 待完成 |
| Attention S x V | `S x V` | Python | HLS C-sim | 待完成 | 待完成 | 待完成 | 待完成 |
| Instruction accelerator | 64-bit micro-instruction dispatch | C++ golden | HLS C-sim | 待完成 | 待完成 | 待完成 | 待完成 |

## HLS 综合结果

| Module | LUT | FF | DSP | BRAM_18K | Latency | II / Interval | Fmax / Timing |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `gemm_top` / `gemm_tiled` | 4931 | 3820 | 16 | 2 | 顶层因运行时 `N/K/M` 显示 `?` | 核心循环均满足 `II=1` | Estimated clock 7.050 ns，约 141.84 MHz |
| `qkv_top` / `qkv_projection` | 5080 | 3827 | 16 | 2 | 顶层因运行时 `N/D` 显示 `?` | `gemm_tiled` 核心循环满足 `II=1` | Estimated clock 7.300 ns，约 136.99 MHz |

关键综合输出：

```text
Pipelining result : Target II = 1, Final II = 1, loop 'dot_k'
**** Loop Constraint Status: All loop constraints were satisfied.
**** Estimated Fmax: 141.84 MHz   # gemm_top
**** Estimated Fmax: 136.99 MHz   # qkv_top
```

## 迭代日志

| 日志 | 内容 |
| --- | --- |
| `docs/iteration_001_minimal_gemm.md` | 最小 `4x4` GEMM，学习 `ap_int`、`ARRAY_PARTITION`、`UNROLL`、`PIPELINE` 和单元 top 接口。 |
| `docs/iteration_002_tiled_gemm.md` | 扩展到 tiled GEMM，发现直接读 B 时容易被存储端口限制。 |
| `docs/iteration_003_buffer_boundary_quant.md` | 合并记录片上 buffer、边界补 0 和右移量化。 |
| `docs/iteration_004_qkv_projection.md` | 加入 `update_A`，并用 GEMM 实现 QKV projection。 |

## 下一步

1. 写 CNN `1x1 conv` 的 Python baseline、`conv_top()` 和 `tb_conv.cpp`。
2. 在理解 im2col 后，尝试 `3x3 conv + GEMM`。
3. 继续做 Transformer 的 `QK^T` 和 `S x V`。
4. 设计 64-bit micro-instruction 格式和最小 `accelerator_top()`。
5. 用 HLS C-sim 验证指令取指、译码、算子 dispatch。
6. 最后再进入 AXI-Lite / AXI Master / DMA 的上板接口设计。
