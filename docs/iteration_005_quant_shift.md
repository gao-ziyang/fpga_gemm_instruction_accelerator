# 迭代日志 005：加入量化右移缩放

## 本次迭代目标

在 004 的边界处理版本基础上加入量化缩放：

```text
INT8 x INT8 -> INT32 accumulate
C = accumulator >> 8
```

当前测试仍为：

```text
N = 7
K = 6
M = 5
GEMM_OUT_SHIFT = 8
```

## 为什么这么做

老师任务里提到的是 CNN 和 Transformer 的推理计算。实际定点推理中，矩阵乘法通常不会只输出原始累加值，而是需要根据量化 scale 做缩放。这里先用最简单的右移 8 位模拟：

```text
output = accumulator / 256
```

这一步不是完整量化方案，还没有 zero-point、per-channel scale、饱和截断或重新量化到 int8，但它已经覆盖了定点 GEMM 中常见的：

```text
C = (A x B) >> shift
```

这种定点输出形式。

## 本次改动

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_types.h` | 新增 `GEMM_OUT_SHIFT = 8`。 |
| `hls/src/gemm_core.cpp` | 写回 C 时由 `localC` 改为 `localC >> GEMM_OUT_SHIFT`。 |
| `hls/tb/tb_gemm.cpp` | C++ golden 同步执行 `sum >> GEMM_OUT_SHIFT`。 |
| `python/golden/gemm_4x4_baseline.py` | Python baseline 同步执行 `acc >> GEMM_OUT_SHIFT`。 |

## 验证方式

### Python baseline

命令：

```bash
cd /mnt/c/Transformer/gzy_gemm_accel
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

### HLS C simulation

关键输出：

```text
[TB] C from HLS:
      29      -7     -13      -9       4
       1       2     -11     -16     -10
     -22       2      30       4     -13
       1     -16     -10       5      30
      30     -13     -15      -8       9
     -17      28       3     -12     -17
     -17     -13       4      31       3
[TB] Golden:
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

本次加入右移后，C-sim 仍和 Python baseline 完全一致。综合资源和 004 基本一致，因为右移 8 位对硬件来说主要是连线和符号扩展，不会像乘法那样消耗 DSP。

现在的 `gemm_tiled` 已经具备老师任务一中 GEMM 微核最核心的几个点：

1. `INT8 x INT8` 输入。
2. `INT32` 中间累加。
3. `4x4` tile 微核。
4. 沿 K 维分块累加。
5. 片上 BRAM buffer 和局部完全 partition。
6. 非整除边界补 0。
7. 输出右移模拟量化 scale。
