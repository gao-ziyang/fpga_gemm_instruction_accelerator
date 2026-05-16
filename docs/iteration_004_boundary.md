# 迭代日志 004：加入边界处理

## 本次迭代目标

让 GEMM 支持 `N/K/M` 不是 `GEMM_TILE` 或 `GEMM_BLOCK_M` 整数倍的情况。本次测试故意设置：

```text
N = 7
K = 6
M = 5
GEMM_TILE = 4
GEMM_BLOCK_M = 8
```

因此最后一块行 tile、列 tile、K tile 都会遇到越界位置。

## 为什么这么做

实际 CNN 和 Transformer 的矩阵形状不一定都能被 tile 大小整除。如果没有边界处理，`i0 + ii`、`k0 + kk`、`j0 + jj` 很容易读到无效位置，轻则 C-sim 结果错误，重则综合出来的硬件行为不可控。

本次采用最容易解释的方式：

1. 加载 `A_bram` 时，`i >= N` 或 `k >= K` 的位置补 0。
2. 加载 `B_bram` 时，`k >= K` 或 `j >= M` 的位置补 0。
3. 加载 `localA/localB` 时，对 tile 内越界位置再补 0。
4. 写回 C 时，只写 `i < N && j < M` 的有效区域。

## 本次改动

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_core.cpp` | `A_bram/B_bram` 加载改为固定上限循环，并对无效位置写 0。 |
| `hls/src/gemm_core.cpp` | `localA/localB` 加载时加入 `i_global/k_global/j_global` 边界判断。 |
| `hls/src/gemm_core.cpp` | `write_c` 只写合法输出位置。 |
| `hls/tb/tb_gemm.cpp` | 测试规模改为 `N=7,K=6,M=5`。 |
| `python/golden/gemm_4x4_baseline.py` | Python baseline 同步改为 `N=7,K=6,M=5`。 |

## 验证方式

### Python baseline

命令：

```bash
cd /mnt/c/Transformer/gzy_gemm_accel
python3 python/golden/gemm_4x4_baseline.py
```

关键输出：

```text
[PY] C = A x B:
    7631    -1646    -3115    -2152     1243
     318      679    -2800    -3847    -2462
   -5587      700     7883     1114    -3223
     284    -3887    -2426     1467     7792
    7691    -3226    -3775    -1892     2423
   -4102     7291     1020    -2819    -4226
   -4119    -3184     1207     8030      901
[PY] checksum=56726
```

### HLS C simulation

关键输出：

```text
[TB] C from HLS:
    7631   -1646   -3115   -2152    1243
     318     679   -2800   -3847   -2462
   -5587     700    7883    1114   -3223
     284   -3887   -2426    1467    7792
    7691   -3226   -3775   -1892    2423
   -4102    7291    1020   -2819   -4226
   -4119   -3184    1207    8030     901
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=56726
[TB] PASS
```

### HLS C synthesis

关键输出：

```text
Pipelining result : Target II = 1, Final II = 1, loop 'dot_k'
**** Loop Constraint Status: All loop constraints were satisfied.
**** Estimated Fmax: 137.09 MHz
```

综合报告摘要：

| 指标 | 数值 |
| --- | --- |
| Target clock | 10.00 ns |
| Estimated clock | 7.295 ns |
| Estimated Fmax | 约 137.09 MHz |
| BRAM_18K | 2 |
| DSP | 16 |
| FF | 2829 |
| LUT | 4305 |

## 本次结果分析

边界处理后，非整除 case 已经和 Python baseline 完全对齐：

```text
mismatch_count = 0
max_abs_error  = 0
```

资源变化也符合预期：LUT 从 003 的约 2439 增加到约 4305，主要来自多个边界判断和 HLS 的 if-conversion。与此同时，`dot_k` 仍保持 `II=1`，说明边界判断没有破坏核心乘加流水。

这个版本已经更接近可复用的 GEMM 核，因为它不再要求输入维度必须被 4 整除。

