# 迭代日志 006：加入 update_A 并实现 QKV projection

## 本次迭代目标

本次在 005 的 tiled GEMM 基础上继续做两件事：

1. 优化 GEMM 核心，加入 `update_A` 控制，让 `A_bram` 可以跨连续 GEMM 调用复用。
2. 基于 Transformer 中 Q/K/V 线性投影的计算形式，用当前 `gemm_tiled` 实现：

```text
Q = (X x Wq) >> 8
K = (X x Wk) >> 8
V = (X x Wv) >> 8
```

测试规模：

```text
N = 7
D = 6
X  shape: [7,6]
Wq shape: [6,6]
Wk shape: [6,6]
Wv shape: [6,6]
Q/K/V shape: [7,6]
```

## 为什么加入 update_A

Q/K/V projection 有一个很重要的数据复用特点：

```text
Q = X x Wq
K = X x Wk
V = X x Wv
```

这三次 GEMM 的左矩阵 `X` 完全相同，只有右矩阵权重 `Wq/Wk/Wv` 不同。因此没有必要每次都把 `X` 从外部数组搬进 `A_bram`。

本次加入：

```cpp
bool update_A
```

含义是：

| `update_A` | 行为 |
| --- | --- |
| `true` | 本次 GEMM 先把输入 A 搬入 `A_bram`，再计算。 |
| `false` | 不刷新 `A_bram`，直接复用上一次留下来的 A，只重新加载 B 权重。 |

在 QKV projection 中调用方式为：

```cpp
gemm_tiled(X, Wq, Q,     N, D, D, true);
gemm_tiled(X, Wk, K_out, N, D, D, false);
gemm_tiled(X, Wv, V,     N, D, D, false);
```

也就是说，Q 的计算负责把 `X` 写入 `A_bram`，K/V 只更换 `B_bram` 中的权重块。

注意：`update_A=false` 的前提是 A 的内容和形状没有变化。如果下一次 GEMM 换了新的输入 X，必须重新传 `update_A=true`。

## 本次对 GEMM 核的优化

除了加入 `update_A`，还做了一个小优化：

```cpp
const int current_block_M =
    (j_block + GEMM_BLOCK_M <= M) ? GEMM_BLOCK_M : (M - j_block);
```

之前 B 加载和 `tile_j_loop` 总是按 `GEMM_BLOCK_M` 跑，最后一个 block 即使只有少数有效列，也会做一些无效循环。本次改为只遍历 `current_block_M`，减少小矩阵和尾块情况下的无效搬运与计算控制。

## 本次新增和修改文件

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_core.h` | `gemm_tiled` 和 `gemm_top` 增加 `bool update_A` 参数。 |
| `hls/src/gemm_core.cpp` | `A_bram` 只在 `update_A=true` 时刷新；B 按 `current_block_M` 加载。 |
| `hls/src/gemm_top.cpp` | 顶层透传 `update_A`。 |
| `hls/tb/tb_gemm.cpp` | 调用 `gemm_top(..., true)`，保持 GEMM 单元测试通过。 |
| `hls/src/qkv_projection.h` | 新增 QKV projection 接口声明。 |
| `hls/src/qkv_projection.cpp` | 用三次 `gemm_tiled` 实现 Q/K/V projection，并复用 `A_bram`。 |
| `hls/src/qkv_top.cpp` | 新增 HLS 顶层 `qkv_top`。 |
| `hls/tb/tb_qkv.cpp` | 新增 QKV C simulation testbench。 |
| `python/golden/qkv_projection_baseline.py` | 新增 QKV Python baseline。 |
| `hls/scripts/run_hls_qkv.tcl` | 新增 QKV HLS 自动运行脚本。 |

## 验证方式

### 1. GEMM Python baseline

命令：

```bash
cd /mnt/c/transformer/gzy_gemm_accel
python3 python/golden/gemm_4x4_baseline.py
```

关键输出：

```text
[PY] C = (A x B) >> 8:
      29       -7      -13       -9        4
       1        2      -11      -16      -10
     -22        2       30        4      -13
       1      -16      -10        5       30
      30      -13      -15       -8        9
     -17       28        3      -12      -17
     -17      -13        4       31        3
[PY] checksum=-140
```

### 2. GEMM HLS C simulation

命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
```

关键输出：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=-140
[TB] PASS
INFO: [SIM 211-1] CSim done with 0 errors.
```

### 3. GEMM HLS C synthesis

关键输出：

```text
Pipelining result : Target II = 1, Final II = 1, loop 'dot_k'
**** Loop Constraint Status: All loop constraints were satisfied.
**** Estimated Fmax: 141.84 MHz
```

综合报告摘要：

| 指标 | 数值 |
| --- | --- |
| Target clock | 10.00 ns |
| Estimated clock | 7.050 ns |
| Estimated Fmax | 约 141.84 MHz |
| BRAM_18K | 2 |
| DSP | 16 |
| FF | 3820 |
| LUT | 4931 |

### 4. QKV Python baseline

命令：

```bash
cd /mnt/c/transformer/gzy_gemm_accel
python3 python/golden/qkv_projection_baseline.py
```

关键输出：

```text
[PY] Q = (X x Wq) >> 8:
      24       13      -11      -23       -1       18
       6       16       -3      -24      -12       12
     -13      -11       11       14      -12      -12
      -6      -12      -14       15       13      -10
      26       12      -12      -23        1       18
       9       16       -4      -24      -10       13
     -11      -12       10       14      -11      -11
[PY] K = (X x Wk) >> 8:
      19        8      -20      -15       24       13
      10       20      -18      -23        6       16
     -12      -10       40       -3      -13      -11
      -9      -15       -3       42       -6      -12
      21        7      -21      -14       26       12
      12       19      -19      -22        9       16
     -11      -12       40       -2      -11      -12
[PY] V = (X x Wv) >> 8:
      10      -14      -26       26       15        4
      19        0      -22        5       14       24
     -11       11       15      -13      -11       -9
     -14      -15       13       -5      -11      -17
       9      -16      -26       29       15        1
      17       -2      -22        7       14       22
     -12       10       14      -11      -11      -12
[PY] checksum=93935
```

### 5. QKV HLS C simulation

命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\transformer\gzy_gemm_accel\hls\scripts\run_hls_qkv.tcl
```

关键输出：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=93935
[TB] PASS
INFO: [SIM 211-1] CSim done with 0 errors.
```

### 6. QKV HLS C synthesis

关键输出：

```text
Pipelining result : Target II = 1, Final II = 1, loop 'dot_k'
**** Estimated Fmax: 136.99 MHz
```

综合报告摘要：

| 指标 | 数值 |
| --- | --- |
| Target clock | 10.00 ns |
| Estimated clock | 7.300 ns |
| Estimated Fmax | 约 136.99 MHz |
| BRAM_18K | 2 |
| DSP | 16 |
| FF | 3827 |
| LUT | 5080 |

## 本次结果分析

### 1. 为什么 QKV 仍然只用 16 个 DSP

`qkv_projection` 顺序调用三次 `gemm_tiled`，且 `gemm_tiled` 是 `INLINE off` 的独立函数模块。HLS 会复用同一套 GEMM 硬件依次完成 Q、K、V，而不是复制三份 GEMM。因此 QKV 顶层资源仍然接近单个 GEMM：

```text
DSP = 16
BRAM_18K = 2
```

这符合当前阶段目标：先做可解释、可验证的 QKV projection，不追求三路完全并行。

### 2. update_A 的收益在哪里

如果 Q/K/V 三次 GEMM 都刷新 A，那么每次都需要执行：

```text
X -> A_bram
```

而现在只有第一次 Q projection 刷新：

```text
Q: update_A=true   load X into A_bram, load Wq, compute Q
K: update_A=false  reuse X in A_bram, load Wk, compute K
V: update_A=false  reuse X in A_bram, load Wv, compute V
```

这减少了两次 X 的片上搬运。对于当前小 case，收益主要体现在结构合理；对于后续更大的 `N=16,D=96`，这类复用会更重要。

### 3. 和后续模块集成的关系

本次实现是在自建 `gemm_tiled` 中加入并验证 `update_A` 机制。当前工程先使用较小的 `GEMM_MAX_N/K/M=8`，便于单元验证和综合观察。

在完整 attention 路径中，Q/K/V projection 可以采用同样的数据复用策略：

```cpp
mmult_core(X, Wq, Q_buf, N, D, D, true);
mmult_core(X, Wk, K_buf, N, D, D, false);
mmult_core(X, Wv, V_buf, N, D, D, false);
```

后续需要在完整 Transformer 计算路径中单独回归验证。
