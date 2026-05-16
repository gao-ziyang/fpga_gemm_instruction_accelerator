# fpga_gemm_instruction_accelerator

本项目基于 Vitis HLS 实现一个小型 INT8 GEMM 计算核，并逐步将其用于 CNN 卷积层映射和 Transformer Encoder 中 `Q/K/V projection`、`QK^T`、`S x V` 等矩阵计算模块的验证。

当前阶段从最小 GEMM 出发，按功能逐步加入 tile、片上 buffer、边界处理、量化右移、`update_A` 复用和 QKV projection。这样每一步都能解释清楚，也方便后续把本工程的 GEMM 核扩展到完整 Transformer 计算路径中。

第二个任务暂时作为未来方向保留：CPU/PS 侧通过指令、descriptor 或 micro-op 控制不同网络层映射到 GEMM、Conv、Attention、softmax 等算子。它和 RISC-V/ISA 有概念关联，但当前更应先掌握 AXI-Lite 控制寄存器、AXI DMA、算子 descriptor 和调度状态机。

## 当前状态

已完成：

1. 最小 `4x4` INT8 GEMM。
2. 扩展到 tiled GEMM：`C[N,M] = A[N,K] x B[K,M]`。
3. 加入 `A_bram`、`B_bram` 和完全 partition 的 `localA/localB/localC`。
4. 支持 `N/K/M` 非 tile 整数倍时的补 0 边界处理。
5. 支持 `INT8 x INT8 -> INT32 accumulate -> >> 8` 的量化右移输出。
6. 加入 `update_A`，支持连续 GEMM 调用时复用 `A_bram`。
7. 基于当前 GEMM 实现 QKV projection：`Q=XWq`、`K=XWk`、`V=XWv`。
8. 使用 Python baseline 和 Vitis HLS C-sim 对齐验证。
9. 使用 Vitis HLS C synthesis 获得资源和时序报告。

当前最新验证 case：

```text
X[7,6] x Wq/Wk/Wv[6,6] -> Q/K/V[7,6]
Q/K/V = (X x W*) >> 8
mismatch_count = 0
max_abs_error  = 0
checksum       = 93935
```

## 任务目标

### 必须完成

1. 跑通完整 HLS Transformer 计算路径。
2. 抽取并理解 `mmult_core` GEMM 微核。
3. 完成 `mmult_core` / `mmult_qkt` / `mmult_sv` 的单元级验证。
4. 使用 Python baseline 对比 HLS C-sim 输出。
5. 说明 CNN Conv 和 Transformer 子模块如何映射到 GEMM。

### 可选增强

1. CNN `im2col` demo。
2. `TILE_SIZE` 参数对比。
3. HLS synthesis 资源报告。
4. C/RTL co-simulation。
5. PS-PL 上板验证。

### 未来工作

1. AXI DMA。
2. 更大 TILE。
3. softmax / LayerNorm 优化。
4. 完整模型部署。

## 工程目录

```text
gzy_gemm_accel/
  README.md
  .gitignore
  hls/
    src/
      gemm_types.h       # 当前 GEMM 的尺寸、tile、量化右移和 ap_int 类型
      gemm_core.h        # gemm_tiled 与 gemm_top 的函数声明
      gemm_core.cpp      # tiled GEMM 核心实现
      gemm_top.cpp       # HLS 顶层函数与接口 pragma
      qkv_projection.h   # QKV projection 函数声明
      qkv_projection.cpp # 使用 gemm_tiled 计算 Q/K/V
      qkv_top.cpp        # QKV HLS 顶层函数
    tb/
      tb_gemm.cpp        # Vitis HLS C simulation testbench
      tb_qkv.cpp         # QKV projection C simulation testbench
    scripts/
      run_hls_gemm.tcl   # 自动打开 HLS 工程并引用外部源码运行 csim/csynth
      run_hls_qkv.tcl    # 自动运行 qkv_top 的 csim/csynth
  python/
    golden/
      gemm_4x4_baseline.py  # Python 参考 baseline，当前已扩展为 tiled GEMM case
      qkv_projection_baseline.py # QKV projection Python baseline
  docs/
    iteration_001_minimal_gemm.md
    iteration_002_tiled_gemm.md
    iteration_003_onchip_buffer.md
    iteration_004_boundary.md
    iteration_005_quant_shift.md
    iteration_006_update_a_qkv.md
  reports/               # 后续手动整理后的报告摘要
  vitis_hls_project/
    mini_gemm_accel/     # Vitis HLS GUI 工程和工具生成产物，本地保留，不上传
```

源码的维护位置是 `hls/src`、`hls/tb`、`hls/scripts` 和 `python/golden`。`vitis_hls_project/` 是工具生成目录，已被 `.gitignore` 忽略，不作为仓库源码上传。

## 当前功能文件说明

| 文件 | 作用 |
| --- | --- |
| `hls/src/gemm_types.h` | 定义 `GEMM_MAX_N/K/M`、`GEMM_TILE`、`GEMM_BLOCK_M`、`GEMM_OUT_SHIFT` 和 `ap_int` 类型。 |
| `hls/src/gemm_core.h` | 声明 `gemm_tiled` 和 HLS 顶层 `gemm_top`。 |
| `hls/src/gemm_core.cpp` | 实现 tiled GEMM、片上 buffer、`update_A`、边界补 0 和输出右移。 |
| `hls/src/gemm_top.cpp` | GEMM HLS 顶层封装，设置 `ap_memory` 数组接口和 `ap_ctrl_hs` 控制接口。 |
| `hls/src/qkv_projection.cpp` | 用三次 `gemm_tiled` 实现 Q/K/V projection，其中 K/V 复用 Q 阶段写入的 `A_bram`。 |
| `hls/src/qkv_top.cpp` | QKV HLS 顶层封装。 |
| `hls/tb/tb_gemm.cpp` | C++ testbench，生成固定 A/B，调用 `gemm_top`，用 C++ golden 比较输出。 |
| `hls/tb/tb_qkv.cpp` | QKV C++ testbench，生成 X/Wq/Wk/Wv，分别比较 Q/K/V。 |
| `python/golden/gemm_4x4_baseline.py` | 纯 Python baseline，不依赖 NumPy/PyTorch，用于独立确认矩阵乘和右移结果。 |
| `python/golden/qkv_projection_baseline.py` | QKV Python baseline，用于验证 `Q/K/V=(XW*)>>8`。 |
| `hls/scripts/run_hls_gemm.tcl` | 自动 reset HLS 工程，添加外部源码，运行 `csim_design` 和 `csynth_design`。 |
| `hls/scripts/run_hls_qkv.tcl` | 自动 reset QKV HLS 工程，设置 `qkv_top` 并运行 `csim_design` 和 `csynth_design`。 |

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

当前没有使用 `uv` 管理环境，没有使用 PyTorch、cocotb、iverilog、Verilator 或 Vivado RTL 仿真。当前验证只依赖 Python 解释器和 Vitis HLS 的 C simulation / C synthesis。

## 当前 GEMM 核心参数

当前自建工程参数位于 `hls/src/gemm_types.h`：

| 参数 | 值 | 含义 |
| --- | --- | --- |
| `GEMM_MAX_N` | 8 | 当前 HLS 数组行维度综合上限 |
| `GEMM_MAX_K` | 8 | 当前 K 维综合上限 |
| `GEMM_MAX_M` | 8 | 当前输出列维度综合上限 |
| `GEMM_TILE` | 4 | GEMM 微核局部 tile 大小 |
| `GEMM_BLOCK_M` | 8 | B/C 矩阵按输出列缓存的块宽 |
| `GEMM_OUT_SHIFT` | 8 | 输出右移位数，模拟定点量化 scale |
| `gemm_data_t` | `ap_int<8>` | A/B 输入数据类型 |
| `gemm_acc_t` | `ap_int<32>` | C 输出和中间累加数据类型 |

`GEMM_MAX_*` 是综合时固定数组上限；`N/K/M` 是运行时传入的实际规模；`GEMM_TILE` 是局部计算 tile 大小。当前为了学习和快速综合，最大尺寸暂设为 8，后续会向目标 Transformer 模块的 `16/96/96` 规模扩展。

`update_A` 是运行时控制参数：当它为 `true` 时，GEMM 会先把左矩阵 A 搬入持久化的 `A_bram`；当它为 `false` 时，GEMM 跳过 A 的搬运，直接复用上一次保存在 `A_bram` 中的 A。它适合 Q/K/V projection 这种左矩阵 `X` 相同、右矩阵 `Wq/Wk/Wv` 不同的连续 GEMM。

## Transformer 目标参数

以下参数用于后续扩展和对齐 `mmult_core`、`mmult_qkt`、`mmult_sv` 等模块：

| 参数 | 值 | 含义 |
| --- | --- | --- |
| `MAX_N` | 16 | token / 行维度综合上限 |
| `MAX_K` | 96 | K / 特征维度综合上限 |
| `MAX_M` | 96 | M / 输出列维度综合上限 |
| `MAX_C` | 10 | 分类头类别数上限 |
| `TILE_SIZE` | 4 | GEMM 微核局部 tile 大小 |
| `BLOCK_M` | 16 | B/C 矩阵按输出列分块的块宽 |
| `DTYPE_IN` | `ap_int<8>` | 输入和权重数据类型 |
| `DTYPE_OUT` | `ap_int<32>` | 常规 GEMM 输出数据类型 |
| `DTYPE_ACC` | `ap_int<64>` | Attention score / 累加路径数据类型 |

## 核心模块说明

| 模块 | 功能 | 数学含义 | 当前状态 |
| --- | --- | --- | --- |
| `gemm_tiled` | 当前自建 tiled GEMM | `C = (A x B) >> 8`，支持 `update_A` | 已完成 001-006 |
| `gemm_top` | GEMM HLS 顶层封装 | 调用 `gemm_tiled(A,B,C,N,K,M,update_A)` | 已完成 |
| `qkv_projection` | Transformer Q/K/V 线性投影 | `Q/K/V = (X x Wq/Wk/Wv) >> 8` | 已完成 |
| `qkv_top` | QKV HLS 顶层封装 | 调用 `qkv_projection` | 已完成 |
| `mmult_core` | 通用 GEMM 目标模块 | `C = (A x B) >> 8` | 待抽取和单元验证 |
| `mmult_qkt` | Attention score | `S = (Q x K^T) >> 14` | 待单元验证 |
| `mmult_sv` | Attention value aggregation | `O = (S >> 28) x V` | 待单元验证 |
| `softmax_sv_fused_res1_lut` | softmax 近似 + `S x V` + residual | `Z = softmax(S) x V + X` | 待分析和验证 |
| `transformer_top` | 顶层 Transformer 流程 | input linear + attention + head | 待完成 |

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

这还不是完整 systolic array，也没有 AXI DMA，但已经具备 FPGA GEMM 微核的基本结构：tile 分块、K 维累加、片上数据复用、局部并行 MAC、边界补 0 和定点缩放。

## 如何运行

### 1. Run GEMM Python baseline

```bash
cd /mnt/c/transformer/gzy_gemm_accel
python3 python/golden/gemm_4x4_baseline.py
```

### 2. Run QKV Python baseline

```bash
cd /mnt/c/transformer/gzy_gemm_accel
python3 python/golden/qkv_projection_baseline.py
```

### 3. Run GEMM HLS C simulation and C synthesis

在 Windows 或能访问 Windows Vitis HLS 的 WSL shell 中运行：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
```

脚本会执行：

```tcl
csim_design
csynth_design
```

### 4. Run QKV HLS C simulation and C synthesis

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\transformer\gzy_gemm_accel\hls\scripts\run_hls_qkv.tcl
```

### 5. 查看综合报告

GEMM 报告路径：

```text
gzy_gemm_accel/vitis_hls_project/mini_gemm_accel/solution1/syn/report/gemm_top_csynth.rpt
```

QKV 报告路径：

```text
gzy_gemm_accel/vitis_hls_project/qkv_projection_accel/solution1/syn/report/qkv_top_csynth.rpt
```

生成的 RTL 路径：

```text
gzy_gemm_accel/vitis_hls_project/mini_gemm_accel/solution1/syn/verilog/
gzy_gemm_accel/vitis_hls_project/qkv_projection_accel/solution1/syn/verilog/
```

## 验证结果

README 只记录当前主要结果；完整过程见 `docs/iteration_*.md`。

| Case | 维度 | Baseline | HLS C-sim | 结果 | max_abs_error | mismatch_count | checksum |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GEMM basic | `A[4,4] x B[4,4]` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | 见日志 001 |
| Tiled GEMM | `A[8,8] x B[8,8]` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | 358080 |
| On-chip buffer GEMM | `A[8,8] x B[8,8]` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | 358080 |
| Boundary GEMM | `A[7,6] x B[6,5]` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | 56726 |
| Quantized GEMM | `A[7,6] x B[6,5]`，`>> 8` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | -140 |
| GEMM with `update_A` | `A[7,6] x B[6,5]`，`update_A=true` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | -140 |
| QKV projection | `X[7,6] x Wq/Wk/Wv[6,6]`，`>> 8` | Python / C++ golden | HLS C-sim | PASS | 0 | 0 | 93935 |
| CNN Conv via GEMM | 计划：先做 `1x1 conv`，再做 `3x3 im2col` | Python | HLS C-sim | 待完成 | 待完成 | 待完成 | 待完成 |
| QK^T | `Q[16,96] x K[16,96]^T` | Python | HLS C-sim | 待完成 | 待完成 | 待完成 | 待完成 |
| S x V | `S[16,16] x V[16,96]` | Python | HLS C-sim | 待完成 | 待完成 | 待完成 | 待完成 |
| Transformer top | `N=16,D=96,C=10` | Python / checksum | HLS C-sim | 待完成 | 待完成 | 待完成 | 待完成 |

当前 GEMM 输出：

```text
[TB] C from HLS:
      29      -7     -13      -9       4
       1       2     -11     -16     -10
     -22       2      30       4     -13
       1     -16     -10       5      30
      30     -13     -15      -8       9
     -17      28       3     -12     -17
     -17     -13       4      31       3
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=-140
[TB] PASS
```

当前 QKV 输出摘要：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=93935
[TB] PASS
```

## HLS 综合结果

当前最新版本为 006：tiled GEMM + on-chip buffer + boundary + output shift + `update_A` + QKV projection。

| Module | LUT | FF | DSP | BRAM_18K | Latency | II / Interval | Fmax / Timing |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `gemm_top` / `gemm_tiled` | 4931 | 3820 | 16 | 2 | 顶层因运行时 `N/K/M` 显示 `?` | 核心循环均满足 `II=1` | Estimated clock 7.050 ns，约 141.84 MHz |
| `qkv_top` / `qkv_projection` | 5080 | 3827 | 16 | 2 | 顶层因运行时 `N/D` 显示 `?` | `gemm_tiled` 核心循环满足 `II=1` | Estimated clock 7.300 ns，约 136.99 MHz |
| `mmult_core` | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 |
| `mmult_qkt` | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 |
| `mmult_sv` | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 |
| `transformer_top` | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 | 待完成 |

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
| `docs/iteration_001_minimal_gemm.md` | 最小 `4x4` GEMM，理解 `ap_int`、`ARRAY_PARTITION`、`UNROLL`、`PIPELINE`。 |
| `docs/iteration_002_tiled_gemm.md` | 扩展到 tiled GEMM，暴露 B 读端口瓶颈。 |
| `docs/iteration_003_onchip_buffer.md` | 加入 `A_bram/B_bram` 和局部 partition，使 `dot_k` 达到 `II=1`。 |
| `docs/iteration_004_boundary.md` | 加入非整除边界处理，越界位置补 0。 |
| `docs/iteration_005_quant_shift.md` | 加入 `>> 8` 量化右移，和 Python / HLS C-sim 对齐。 |
| `docs/iteration_006_update_a_qkv.md` | 加入 `update_A` 复用 `A_bram`，并实现 QKV projection。 |

## 后续路线

1. 将当前 `GEMM_MAX_N/K/M=8` 扩展到 `MAX_N=16, MAX_K=96, MAX_M=96`，观察资源和时序。
2. 完成 `mmult_core` 风格接口封装，确认本工程 `gemm_tiled` 与目标 GEMM 模块在数据排布和右移策略上的一致性。
3. 在 attention 计算路径中验证 Q/K/V 调用是否可采用 `true/false/false` 的 `update_A` 复用策略。
4. 增加 `mmult_qkt` 和 `mmult_sv` 的 Python baseline 与 HLS C-sim 单元测试。
5. 增加 CNN `1x1 conv` via GEMM，之后再做 `3x3 conv + im2col`。
6. 跑通单层单头 Transformer 的最小路径，再尝试接入当前 GEMM 核。
7. 第二任务再进入 PS/CPU 控制算子 descriptor、AXI-Lite 控制和 AXI DMA 数据搬运。
