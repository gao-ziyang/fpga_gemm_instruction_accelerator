# 迭代日志 002：扩展到 4x4 tile GEMM

## 本次迭代目标

在 001 的最小 `4x4 GEMM` 基础上，将计算扩展为运行时传入规模的 tiled GEMM：

```text
C[N,M] = A[N,K] x B[K,M]
```

本次设置：

```text
GEMM_MAX_N = 8
GEMM_MAX_K = 8
GEMM_MAX_M = 8
GEMM_TILE  = 4
N = 8
K = 8
M = 8
```

本次只做 tile 拆分和 K 维分块累加，不引入片上 A/B buffer，不处理非整除边界，也不做右移量化。

## 为什么这么做

老师要求的 GEMM 核不可能只停留在固定 `4x4`。后续 CNN 卷积和 Transformer 中的 `Q/K/V projection`、`QK^T`、`S x V` 都需要不同的 `N/K/M`。因此第二步要把最小 tile 扩展成：

```text
for i0 in N step 4:
  for j0 in M step 4:
    localC[4][4] = 0
    for k0 in K step 4:
      localC += A_tile[4][4] x B_tile[4][4]
    C_tile = localC
```

这一步的重点是理解：

1. `C` 被拆成多个 `4x4` 输出 tile。
2. 每个输出 tile 沿 K 维分块累加。
3. `localC` 必须跨多个 K tile 累加，不能每次 K 分块都写回覆盖。

## 本次改动

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_types.h` | 将固定 `GEMM_DIM=4` 扩展为 `GEMM_MAX_N/K/M=8` 和 `GEMM_TILE=4`。 |
| `hls/src/gemm_core.h` | 将 `gemm_4x4` 接口替换为 `gemm_tiled(A, B, C, N, K, M)`。 |
| `hls/src/gemm_core.cpp` | 实现 `i0/j0/k0` 三层 tile 循环和 `localC` 分块累加。 |
| `hls/src/gemm_top.cpp` | 顶层增加运行时参数 `N/K/M`。 |
| `hls/tb/tb_gemm.cpp` | 测试规模改为 `8x8x8`，增加 `mismatch_count`、`max_abs_error`、`checksum`。 |
| `python/golden/gemm_4x4_baseline.py` | baseline 改为 `8x8x8` GEMM。 |
| `hls/scripts/run_hls_gemm.tcl` | 使用 `open_project -reset` 和 `open_solution -reset`，避免重复添加文件导致综合失败。 |

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
    5916    -2984    -4076    -2736     -628      200    -3324    -2496
    1992     1164    -3504    -5740     -296     8604     1504    -5980
   -6540      704     8844     3032    -4572    -3984     1724     2312
   -3296    -4364      200     7196     2928    -4796    -2664     5996
   10572    -2520    -5244    -5536     6204     2840    -2316    -5680
   -3848     7516     1216    -2652    -3960      748     8400    -3276
   -6492    -3440     3068    12008    -2348    -5952    -1876    10904
   11728     -956    -5448    -7508     6816     3860    -3064    -6020
[PY] checksum=358080
```

### HLS C simulation

命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
```

关键输出：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=358080
[TB] PASS
INFO: [SIM 211-1] CSim done with 0 errors.
```

### HLS C synthesis

本次综合通过，但出现一个重要现象：

```text
WARNING: Unable to schedule 'load' operation on array 'B' due to limited memory ports.
Pipelining result : Target II = 1, Final II = 2, loop 'tile_k_loop_dot_k'
**** Loop Constraint Status: All loop constraints were NOT satisfied.
**** Estimated Fmax: 146.48 MHz
```

## 本次结果分析

这次的数学结果正确，但调度结果暴露了访存瓶颈：`dot_k` 里要并行读多个 `B[k][j]`，而外部数组接口或普通存储端口数量有限，HLS 无法在一个周期内满足所有读请求，所以 `II` 被迫从目标 `1` 放宽到 `2`。

这个问题正好引出下一次迭代：把 A/B 先搬到片上 buffer，再把 `localA/localB/localC` 完全 partition，让局部计算不再直接被外部 B 矩阵端口卡住。

