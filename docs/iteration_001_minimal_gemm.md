# 迭代日志 001：最小 4x4 INT8 GEMM

## 本次迭代目标

本次迭代从最小 GEMM 出发，只实现：

```text
C[4,4] = A[4,4] x B[4,4]
```

暂时不考虑大矩阵分块、`BLOCK_M`、运行时 `N/K/M` 参数、AXI DMA、CNN `im2col`、Transformer Attention 或完整模型。这样做的目的是先把最小 HLS 计算核、C simulation、Python baseline 和 C synthesis report 的链路跑通，避免一上来就写过大的综合模块，导致代码难以解释。

本次重点理解：

1. `ap_int` 定点整数类型。
2. `ARRAY_PARTITION` 如何把小数组拆成并行寄存器访问。
3. `UNROLL` 如何展开 k 维 dot-product。
4. `PIPELINE` 如何让循环形成流水。
5. Vitis HLS C simulation 如何调用 `.cpp` testbench。
6. Python baseline 如何独立验证矩阵乘结果。

## 本次新增文件

| 文件 | 作用 |
| --- | --- |
| `hls/src/gemm_types.h` | 定义 `GEMM_DIM=4`、`gemm_data_t=ap_int<8>`、`gemm_acc_t=ap_int<32>`。 |
| `hls/src/gemm_core.h` | 声明 `gemm_4x4` 和 `gemm_top`。 |
| `hls/src/gemm_core.cpp` | 实现最小 4x4 GEMM 计算核。 |
| `hls/src/gemm_top.cpp` | HLS 顶层函数，设置 `ap_memory` 和 `ap_ctrl_hs` 接口。 |
| `hls/tb/tb_gemm.cpp` | HLS C simulation testbench。 |
| `python/golden/gemm_4x4_baseline.py` | Python 参考 baseline。 |
| `hls/scripts/run_hls_gemm.tcl` | 自动引用外部源码，运行 C simulation 和 C synthesis。 |
| `.gitignore` | 忽略 Vitis HLS 工具生成目录和缓存。 |
| `README.md` | 项目总说明。 |

## 为什么这么做

### 1. 先写最小 GEMM，而不是直接写完整 `mmult_core`

完整 `mmult_core` 需要包含 tile、BRAM 缓存、`BLOCK_M` 分块、运行时 `N/K/M` 参数和右移量化。它适合作为后续目标，但不适合作为第一份“自己能解释清楚”的代码。

因此本次先写最小 `4x4`：

```text
A: 4x4 int8
B: 4x4 int8
C: 4x4 int32
```

这样每个输出元素都是 4 项乘加：

```text
C[i][j] = A[i][0]B[0][j] + A[i][1]B[1][j] + A[i][2]B[2][j] + A[i][3]B[3][j]
```

这正好对应后续 tiled GEMM 里的一个最小局部计算块。

### 2. 为什么使用 `ap_int<8>` 和 `ap_int<32>`

老师任务中数据精度可以选择 `INT8 x INT8`。本次使用：

```cpp
typedef ap_int<8>  gemm_data_t;
typedef ap_int<32> gemm_acc_t;
```

`int8 x int8` 单次乘法最大约为 `127 x 127`，4 项累加远小于 32 位范围。因此 `ap_int<32>` 对本次最小 GEMM 足够，并且和后续 `mmult_core` 的 `DTYPE_OUT=ap_int<32>` 保持方向一致。

### 3. 为什么使用 `ARRAY_PARTITION complete`

代码中：

```cpp
#pragma HLS ARRAY_PARTITION variable=localA complete dim=0
#pragma HLS ARRAY_PARTITION variable=localB complete dim=0
```

本次 `localA/localB` 都只有 `4x4`，完全拆分后每个元素都可以并行访问，适合配合 `dot_loop` 的 `UNROLL`。这能帮助理解“小型 Tensor Core / MMA tile”在 FPGA 上的基本形式：用寄存器阵列保存小 tile，用并行乘加计算局部输出。

### 4. 为什么 `dot_loop` 使用 `UNROLL`

代码中：

```cpp
for (int k = 0; k < GEMM_DIM; k++) {
#pragma HLS UNROLL
    sum += localA[i][k] * localB[k][j];
}
```

`k` 维只有 4 项，完全展开后可以并行形成乘加结构。综合日志也显示该 loop 被完全展开：

```text
Unrolling loop 'dot_loop' ... completely with a factor of 4
```

### 5. 为什么 `load_mats` 和 `col_loop` 使用 `PIPELINE`

`load_mats` 用于把输入数组读入局部寄存器阵列；`col_loop` 用于逐个计算输出元素。给循环加 `PIPELINE II=1` 是为了观察 HLS 如何调度这些循环，并为后续更大 GEMM 的吞吐优化做准备。

本次综合报告显示：

```text
Pipelining result : Target II = 1, Final II = 1, loop 'load_mats'
Pipelining result : Target II = 1, Final II = 1, loop 'col_loop'
```

说明当前两个 pipeline 约束都满足。

## 本次验证方式

### 1. Python baseline

命令：

```bash
cd /mnt/c/Transformer
python3 gzy_gemm_accel/python/golden/gemm_4x4_baseline.py
```

终端输出：

```text
[PY] A:
       1       -2        3        4
       5        6       -7        8
      -1        2       -3        4
       9       -8        7       -6
[PY] B:
       1        2       -3        4
      -5        6        7       -8
       9      -10       11       12
     -13       14      -15       16
[PY] C = A x B:
     -14       16      -44      120
    -192      228     -170       16
     -90       96      -76        8
     190     -184       84       88
```

### 2. HLS C simulation

命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
```

Tcl 脚本会打开本地 HLS 工程：

```text
gzy_gemm_accel/vitis_hls_project/mini_gemm_accel
```

并引用外部源码：

```text
C:/Transformer/gzy_gemm_accel/hls/src/gemm_core.cpp
C:/Transformer/gzy_gemm_accel/hls/src/gemm_top.cpp
C:/Transformer/gzy_gemm_accel/hls/tb/tb_gemm.cpp
```

HLS C simulation 输出：

```text
INFO: [SIM 2] *************** CSIM start ***************
INFO: [SIM 4] CSIM will launch GCC as the compiler.
   Compiling ../../../../../hls/tb/tb_gemm.cpp in debug mode
   Compiling ../../../../../hls/src/gemm_top.cpp in debug mode
   Compiling ../../../../../hls/src/gemm_core.cpp in debug mode
   Generating csim.exe
[TB] C from HLS:
     -14      16     -44     120
    -192     228    -170      16
     -90      96     -76       8
     190    -184      84      88
[TB] Golden:
     -14      16     -44     120
    -192     228    -170      16
     -90      96     -76       8
     190    -184      84      88
[TB] PASS
INFO: [SIM 1] CSim done with 0 errors.
INFO: [SIM 3] *************** CSIM finish ***************
```

结论：

```text
mismatch_count = 0
max_abs_error  = 0
结果           = PASS
```

### 3. HLS C synthesis

综合环境：

```text
Vitis HLS: 2020.2
Project: mini_gemm_accel
Solution: solution1
Flow: Vivado IP Flow Target
Target device: xc7z020-clg400-2
Clock target: 10 ns
```

综合报告摘要：

```text
Clock target     : 10.00 ns
Estimated clock  : 6.722 ns
Latency          : 56 cycles / 0.560 us
Interval         : 57
BRAM_18K         : 0
DSP              : 2
FF               : 502
LUT              : 485
Estimated Fmax   : about 148.76 MHz
```

资源表：

| Module | LUT | FF | DSP | BRAM | Latency | Timing |
| --- | --- | --- | --- | --- | --- | --- |
| `gemm_top` / `gemm_4x4` | 485 | 502 | 2 | 0 | 56 cycles | Estimated clock 6.722 ns |

## 本次结果分析

### 1. 为什么 BRAM 为 0

本次矩阵只有 `4x4`，`localA/localB` 又被 `ARRAY_PARTITION complete` 完全拆分，HLS 会把它们实现成寄存器和组合选择逻辑，而不是 BRAM。

### 2. 为什么 DSP 是 2

虽然 `dot_loop` 被 `UNROLL factor=4`，但 HLS 仍然会结合 pipeline 调度和资源共享进行绑定，不一定直接生成 4 个独立 DSP。当前综合结果是 2 个 DSP，说明 HLS 对乘法资源做了一定共享。

这也提示后续分析不能只看 C++ 代码里的 unroll，还要看 HLS binding 和 synthesis report。

### 3. 为什么 latency 是 56 cycles

56 cycles 包括：

1. 读取 A/B 到局部数组。
2. 计算 16 个输出元素。
3. 写回 C。
4. 函数控制和流水线调度开销。

本次没有追求极致 latency，而是优先追求结构清楚、验证闭环完整。

### 4. 当前硬件结构如何理解

本次硬件大致是：

```text
A/B ap_memory 接口
  -> localA/localB 寄存器阵列
  -> 每个 C[i][j] 的 4 项并行/部分并行乘加
  -> C ap_memory 写回
```

这不是最终的大矩阵 tiled GEMM，也不是 systolic array，而是最小 GEMM tile 的教学版原型。

## 本次没有做的事情

1. 没有直接写完整 `mmult_core`。
2. 没有做 `N/K/M` 运行时可变参数。
3. 没有做 `BLOCK_M`、大矩阵分块或片上 BRAM 缓存。
4. 没有做 CNN `im2col`。
5. 没有做 `QK^T`、`S x V` 或 Transformer top。
6. 没有做 C/RTL co-simulation。
7. 没有用 Vivado 打开 RTL schematic。
8. 没有用 cocotb、iverilog、Verilator、PyTorch 或 uv。

## 后续迭代计划

下一步建议：

1. 把 `gemm_4x4` 扩展成可配置 `M/N/K` 的小 GEMM。
2. 逐步靠近完整 `mmult_core`：增加 tile、局部缓存、输出右移。
3. 为 `mmult_core` 写 Python baseline 和 HLS C-sim 对比。
4. 再做 `mmult_qkt` 和 `mmult_sv` 的单元级验证。
5. 最后再进入 CNN Conv 映射和 Transformer Encoder 子模块。
