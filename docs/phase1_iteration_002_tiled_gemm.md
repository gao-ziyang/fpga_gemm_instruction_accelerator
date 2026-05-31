# Phase 1 / 迭代日志 002：从固定 4x4 扩展到 tiled GEMM

## 我这一版想解决什么

上一版只能算固定 `4x4`，这一版我想让 GEMM 至少能支持运行时传入：

```text
C[N,M] = A[N,K] x B[K,M]
```

当前测试先用：

```text
GEMM_MAX_N = 8
GEMM_MAX_K = 8
GEMM_MAX_M = 8
GEMM_TILE  = 4
N = 8
K = 8
M = 8
```

这样可以先理解“大矩阵拆成多个 4x4 tile”这件事。

## 我改了哪些地方

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_types.h` | 从固定 `GEMM_DIM=4` 改成 `GEMM_MAX_N/K/M=8` 和 `GEMM_TILE=4`。 |
| `hls/src/gemm_core.h` | 接口改成 `gemm_tiled(A,B,C,N,K,M)`。 |
| `hls/src/gemm_core.cpp` | 增加 `i0/j0/k0` 三层 tile 循环。 |
| `hls/src/gemm_top.cpp` | 顶层增加 `N/K/M` 参数。 |
| `hls/tb/tb_gemm.cpp` | 测试从 `4x4` 改成 `8x8x8`。 |
| `python/golden/gemm_4x4_baseline.py` | Python baseline 同步改成 tiled GEMM case。 |

## 我学到的东西

这一版我第一次把 GEMM 写成 tile 形式：

```text
for i0 in N step 4:
  for j0 in M step 4:
    localC[4][4] = 0
    for k0 in K step 4:
      localC += A_tile[4][4] x B_tile[4][4]
    C_tile = localC
```

这里我比较容易出错的地方是：`localC` 不能在每个 `k0` 分块里重新清零。它必须跨多个 K tile 累加，最后才写回 C。

这也让我开始意识到，GEMM 不是简单三重循环，后面真正难的地方会变成：

```text
数据从哪里读；
读几次；
能不能复用；
读端口够不够；
并行计算能不能喂饱。
```

## 验证过程

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

关键输出：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=358080
[TB] PASS
INFO: [SIM 211-1] CSim done with 0 errors.
```

### HLS C synthesis

这一版综合能通过，但是报告里出现了一个很重要的问题：

```text
WARNING: Unable to schedule 'load' operation on array 'B' due to limited memory ports.
Pipelining result : Target II = 1, Final II = 2, loop 'tile_k_loop_dot_k'
**** Loop Constraint Status: All loop constraints were NOT satisfied.
**** Estimated Fmax: 146.48 MHz
```

## 这一版的问题和后续想法

数学结果是对的，但综合报告提醒我：计算循环里要同时读很多 `B[k][j]`，而普通数组接口或未拆分的存储端口不够，所以 HLS 没办法把目标 `II=1` 调度出来。

我理解这里的问题不是“乘法算不动”，而是“数据供不上”。后面应该尝试先把 A/B 搬到片上 buffer，再把当前 `4x4` tile 搬到完全 partition 的 local buffer 中，让真正计算阶段不要直接受外部数组端口限制。
