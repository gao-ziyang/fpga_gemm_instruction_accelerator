# 迭代日志 004：加入 update_A，并用 GEMM 实现 QKV projection

> 说明：这一篇记录的是当时的 QKV 学习版本，所以公式里还写了固定 `>> 8`。后来在迭代 007 里我把 GEMM core 改成只输出原始 INT32 累加值，Q/K/V 如果要继续参与 Attention GEMM，再由 `saturate_to_int8(x, shift)` 做可配置量化。

## 我这一版想解决什么

上一版 GEMM 核已经能处理 tile、buffer、边界和右移量化。这一版我想把它放到 Transformer 里最容易切入的部分：

```text
Q = (X x Wq) >> 8
K = (X x Wk) >> 8
V = (X x Wv) >> 8
```

这三个计算本质上都是 GEMM，而且左矩阵 `X` 是一样的。所以我又加了一个 `update_A` 参数，尝试复用 `A_bram` 里的 `X`。

## 我改了哪些地方

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_core.h` | `gemm_tiled` 和 `gemm_top` 增加 `bool update_A` 参数。 |
| `hls/src/gemm_core.cpp` | `A_bram` 只在 `update_A=true` 时刷新。 |
| `hls/src/gemm_top.cpp` | 顶层透传 `update_A`。 |
| `hls/tb/tb_gemm.cpp` | GEMM 单元测试调用 `gemm_top(..., true)`。 |
| `hls/src/qkv_projection.h` | 新增 QKV projection 函数声明。 |
| `hls/src/qkv_projection.cpp` | 用三次 `gemm_tiled` 实现 Q/K/V。 |
| `hls/src/qkv_top.cpp` | 新增 QKV HLS 顶层。 |
| `hls/tb/tb_qkv.cpp` | 新增 QKV C simulation testbench。 |
| `python/golden/qkv_projection_baseline.py` | 新增 QKV Python baseline。 |
| `hls/scripts/run_hls_qkv.tcl` | 新增 QKV HLS 自动运行脚本。 |

## 我学到的东西

### update_A 的意义

Q/K/V projection 的形式是：

```text
Q = X x Wq
K = X x Wk
V = X x Wv
```

所以三次 GEMM 的左矩阵 `X` 不变。`update_A` 的意思是：

| `update_A` | 行为 |
| --- | --- |
| `true` | 把当前 A 搬进 `A_bram`。 |
| `false` | 不重新搬 A，直接复用上一次 `A_bram` 里的内容。 |

当前 QKV 里这样调用：

```cpp
gemm_tiled(X, Wq, Q,     N, D, D, true);
gemm_tiled(X, Wk, K_out, N, D, D, false);
gemm_tiled(X, Wv, V,     N, D, D, false);
```

这不会减少 BRAM 数量，`A_bram` 还是存在；它减少的是两次重复搬运 `X` 的时间和外部读流量。当前小 case 里收益不明显，但结构上比较接近 QKV 这种连续线性层的数据复用。

我也意识到，如果以后为了性能把 Q/K/V 三路完全并行，各自一套 GEMM，那么这个 `update_A` 就不一定有收益。另一种更常见的方式可能是把权重拼成：

```text
X x [Wq Wk Wv]
```

一次更宽的 GEMM 直接算出 QKV。当前版本先做串行复用，是因为资源更省，也更容易验证。

### qkv_top 和 qkv_projection 的关系

这一版我把它们分成两层：

```text
qkv_top：HLS 顶层，主要负责接口 pragma。
qkv_projection：算法函数，负责调用三次 gemm_tiled。
```

所以结构是：

```text
qkv_top
  -> qkv_projection
      -> gemm_tiled
```

这样综合报告里也能看到 `qkv_top`、`qkv_projection`、`gemm_tiled` 的层次。

### INLINE off

`gemm_tiled` 和 `qkv_projection` 里使用：

```cpp
#pragma HLS INLINE off
```

我的理解是先保留函数边界，不让 HLS 把函数直接揉进上层。这样报告更容易看，也更像一个可以被上层调用的计算模块。

## 验证过程

### GEMM 回归

先确认改了 `update_A` 之后，原来的 GEMM 单元测试没有坏：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=-140
[TB] PASS
INFO: [SIM 211-1] CSim done with 0 errors.
```

GEMM 综合摘要：

```text
Pipelining result : Target II = 1, Final II = 1, loop 'dot_k'
**** Loop Constraint Status: All loop constraints were satisfied.
**** Estimated Fmax: 141.84 MHz
BRAM_18K = 2
DSP      = 16
FF       = 3820
LUT      = 4931
```

### QKV Python baseline

命令：

```bash
cd /mnt/c/Transformer/gzy_gemm_accel
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
[PY] checksum=93935
```

### QKV HLS C simulation

命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_qkv.tcl
```

关键输出：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=93935
[TB] PASS
INFO: [SIM 211-1] CSim done with 0 errors.
```

### QKV HLS C synthesis

关键输出：

```text
Pipelining result : Target II = 1, Final II = 1, loop 'dot_k'
**** Estimated Fmax: 136.99 MHz
BRAM_18K = 2
DSP      = 16
FF       = 3827
LUT      = 5080
```

## 这一版的问题和后续想法

这一版 QKV 是串行调用三次 `gemm_tiled`，所以资源接近单个 GEMM：

```text
DSP = 16
BRAM_18K = 2
```

这说明 HLS 没有复制三套 GEMM，而是在同一套硬件上依次算 Q、K、V。这样资源省，但时间会比三路并行更长。

后续我需要继续补两块内容：

1. CNN 卷积层怎么映射到 GEMM。
2. Attention 里的 `QK^T` 和 `S x V` 怎么继续接到当前 GEMM 结构上。

同时我对任务 2 的理解也有了变化：它更像是定义固定宽度的 micro-instruction，例如 64-bit 指令字，用 opcode 和 operand 描述不同网络层，然后在 `accelerator_top()` 里取指、译码、调用 GEMM/Conv/Attention 子模块。这部分后面应该单独实现和验证。
最后再考虑上板子吧？
