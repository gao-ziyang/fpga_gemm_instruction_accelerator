# 迭代日志 003：片上 buffer、边界处理和右移量化

## 我这一版想解决什么

上一版 tiled GEMM 的数值是对的，但综合报告显示 `B` 的读端口会限制 `II=1`。这一版我开始补更像 FPGA GEMM 的结构：

```text
外部 A/B
  -> A_bram / B_bram
  -> localA / localB / localC
  -> 4x4 并行 MAC
  -> C
```

这个过程中我又顺手把两个遗漏补上了：

```text
N/K/M 不是 4 的整数倍时，需要边界处理；
INT8 GEMM 的输出需要右移缩放，先用 >> 8 模拟量化 scale。
```

所以这一版其实是 GEMM 核从“能算”走向“更像可复用计算核”的一次整理。

## 我改了哪些地方

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_core.cpp` | 增加 `A_bram` 和 `B_bram`。 |
| `hls/src/gemm_core.cpp` | 给 `A_bram/B_bram` 加 `BIND_STORAGE ram_2p impl=bram`。 |
| `hls/src/gemm_core.cpp` | 增加 `localA/localB/localC`，并完全 partition。 |
| `hls/src/gemm_core.cpp` | 在 local tile 加载和写回 C 时加入边界判断。 |
| `hls/src/gemm_types.h` | 增加 `GEMM_BLOCK_M` 和 `GEMM_OUT_SHIFT`。 |
| `hls/tb/tb_gemm.cpp` | 测试规模改成 `N=7,K=6,M=5`，故意覆盖边界 case。 |
| `python/golden/gemm_4x4_baseline.py` | Python baseline 同步加入边界 case 和 `>> 8`。 |

## 我学到的东西

### 为什么要有 A_bram / B_bram

我现在的理解是，BRAM 不是为了直接喂满 16 路 MAC，而是作为片上缓存，让数据先从外部接口搬到 FPGA 内部。真正计算时，还是要把当前 tile 搬到 `localA/localB`。

```text
BRAM 负责片上缓存和复用；
local buffer 负责并行访问；
MAC 阵列负责计算。
```

### 为什么 B_bram 用 GEMM_BLOCK_M

当前工程里：

```text
GEMM_MAX_M = 8
GEMM_BLOCK_M = 8
```

所以当前小 case 下，`GEMM_BLOCK_M` 还没有真正节省 BRAM。它更像是我为后续大矩阵预留的结构。以后如果变成：

```text
GEMM_MAX_K = 96
GEMM_MAX_M = 96
GEMM_BLOCK_M = 16
```

那 `B_bram` 就不用一次缓存完整 `96 x 96`，而是只缓存 `96 x 16` 的列块。

这不会减少从外部读 B 的总数据量，本质上还是要把所有 B 列都读进来；它减少的是片上“同时需要放下的 B”的大小。代价是多了一层 block 控制，以及每个 block 的加载和流水启动开销。

### 为什么 BRAM 到 local 只做 PIPELINE

`A_bram/B_bram` 是双端口 BRAM，不可能一拍直接读出很多个元素去喂 16 路 MAC。所以 BRAM 到 local 的加载阶段主要是：

```cpp
#pragma HLS PIPELINE II=1
```

也就是每周期连续搬一个元素。真正能并行读很多数据的是完全 partition 之后的 `localA/localB/localC`。

### 边界处理

这一版我故意用：

```text
N = 7
K = 6
M = 5
```

这样最后一块 tile 一定会越界。现在的做法是：

```text
读 localA/localB 时，如果越界就补 0；
写 C 时，只写有效的 i < N && j < M。
```

这个方法比较直观，适合第一版验证。

### 右移量化

这一版加入：

```text
C = accumulator >> 8
```

这还不是完整量化，没有 zero-point、per-channel scale、饱和截断，但已经开始接近定点推理里的常见形式：

```text
INT8 x INT8 -> INT32 accumulate -> requant / shift
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
Pipelining result : Target II = 1, Final II = 1, loop 'load_a_bram'
Pipelining result : Target II = 1, Final II = 1, loop 'load_b_bram'
Pipelining result : Target II = 1, Final II = 1, loop 'load_local_a'
Pipelining result : Target II = 1, Final II = 1, loop 'load_local_b'
Pipelining result : Target II = 1, Final II = 1, loop 'dot_k'
Pipelining result : Target II = 1, Final II = 1, loop 'write_c'
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

## 这一版的问题和后续想法

这一版 `dot_k` 能达到 `II=1`，说明“BRAM 缓存 + local 完全 partition”这个方向有效。代价也很明显：BRAM 从 0 变成 2，DSP 到 16，LUT/FF 也增加了。

我现在对资源的理解更清楚了一点：

```text
BRAM：放 A_bram/B_bram 这种片上缓存；
DSP：做 16 路并行乘加；
FF/LUT：做 local 寄存器、控制逻辑、边界判断、mux、pipeline 寄存器。
```

后续可以继续考虑两个方向：

1. 扩大矩阵规模时，`GEMM_BLOCK_M` 是否真的能减少 B buffer 压力。
2. 如果资源太大，是否要减少并行度、降低 tile 大小，或者做更细的数组 partition。
