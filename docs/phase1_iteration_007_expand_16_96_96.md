# Phase 1 / 迭代日志 007：把 GEMM 最大尺寸扩到 16/96/96

## 我这一版想解决什么

前面的 GEMM、Conv、QKV、Attention 都能跑，但最开始的 `GEMM_MAX_N/K/M=8/8/8` 太像小玩具了。老师任务里同时有 CNN 和 Transformer，所以我这一版把最大矩阵容量扩大到：

```text
GEMM_MAX_N = 16
GEMM_MAX_K = 96
GEMM_MAX_M = 96
GEMM_TILE  = 4
```

这次我只扩大最大容量，不扩大 `GEMM_TILE`。

## 为什么先不改 GEMM_TILE

我现在理解是：

```text
GEMM_MAX_N/K/M
  -> 决定片上数组、接口数组、循环上限
  -> 扩大后可以支撑更大的层
  -> 主要增加 buffer 容量和循环次数

GEMM_TILE
  -> 决定局部 4x4 MAC 微核并行度
  -> 从 4 改到 8 会让并行乘加数量明显增加
  -> 资源和时序压力会更大
```

所以这一版我先让容量能覆盖目标算子，暂时保持 4x4 微核不变。这样更像一个稳一点的工程迭代。

## 重新思考 GEMM 里面的固定右移

这一版我又回头看了 GEMM core，发现之前把结果写回时固定写成：

```cpp
C[i][j] = localC[i][j] >> 8;
```

这个写法虽然像量化推理里的 scale，但我现在觉得放在 GEMM core 里面不太合适。GEMM 本身最干净的定义应该是：

```text
C = A x B
INT8 x INT8 -> INT32 accumulate
```

右移、截断、饱和更像是 requantization，也就是量化后处理。它应该在需要把 INT32 中间结果重新变回 INT8 时再单独做，而不是每一次 GEMM 默认偷偷右移 8 位。

所以我这次把 GEMM core 改成：

```cpp
C[i_global][j_global] = localC[ii][jj];
```

然后让 Conv、QKV 的 golden reference 都改回 raw sum。Attention 里因为 Q/K/V 和 Score 还要继续喂给 `gemm_tiled()`，所以继续保留 `saturate_to_int8(x, shift)`，由 `q_shift`、`score_shift`、`p_shift` 这些参数分别控制。这样我的理解会清楚很多：GEMM 是计算单元，shift/saturate 是后处理单元。

## 扩容后能覆盖什么

当前尺寸能覆盖：

```text
Transformer QKV:  [16,96] x [96,96]
Transformer QK^T: [16,96] x [96,16]
Transformer P*V:  [16,16] x [16,96]
CNN Conv:         [16,27] x [27,4]
```

CNN 这一版也顺手扩到了：

```text
Input:  [3,6,6]
Weight: [4,3,3,3]
GEMM:   A[16,27] x B[27,4]
```

这比前面的 `Input[2,3,3]` 更像真实卷积层一点，但还不会把资源压力一下子拉到太离谱。

## 我改了哪些地方

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_types.h` | `GEMM_MAX_N/K/M` 改成 `16/96/96`，`GEMM_TILE` 保持 4。 |
| `hls/src/gemm_core.cpp` | 去掉 GEMM 写回时固定 `>> 8`，输出原始 INT32 累加结果。 |
| `hls/src/conv_types.h` | Conv 参数扩到 `Cin=3,H=W=6,Kh=Kw=3,Cout=4`。 |
| `hls/tb/tb_gemm.cpp` | GEMM golden 改成直接 `sum(A*B)`。 |
| `hls/tb/tb_conv.cpp` | 测试数据按新 Conv shape 生成，并打印 `A[16,27] x B[27,4]`。 |
| `hls/tb/tb_qkv.cpp` | QKV 测试扩到 `N=16,D=96`，golden 改成 raw INT32。 |
| `hls/tb/tb_attention.cpp` | Attention 增加 `N=16,D=96` case，并显式使用 `q_shift=8`、`score_shift=8`。 |
| `README.md` / `README_conv.md` / `README_attention.md` | 更新到当前扩容后的状态。 |

## 验证过程

我把几个模块都重新跑了一遍：

```bash
python3 python/golden/gemm_4x4_baseline.py
python3 python/golden/qkv_projection_baseline.py
```

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_conv.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_qkv.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_score.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_no_softmax.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_attention_hls.tcl
```

## 终端关键结果

### Python baseline

```text
[PY] C = A x B:
[PY] checksum=56726

[PY] Q = X x Wq:
[PY] K = X x Wk:
[PY] V = X x Wv:
[PY] checksum=265116672
```

### GEMM

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=56726
[TB] PASS
INFO: [COSIM 212-1000] *** C/RTL co-simulation finished: PASS ***
```

### Conv2D

```text
[TB] Conv shape: input[3,6,6], weight[4,3,3,3], GEMM A[16,27] x B[27,4]
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=-72952
[PASS] Conv2D -> GEMM validation passed.
INFO: [COSIM 212-1000] *** C/RTL co-simulation finished: PASS ***
```

### QKV

```text
[TB] QKV shape: X[16,96] x W[96,96]
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=265116672
[TB] PASS
INFO: [COSIM 212-1000] *** C/RTL co-simulation finished: PASS ***
```

### Attention

```text
[TB] score case N=16 D=96 mismatch_count=0
[TB] score case N=16 D=96 max_abs_error=0
[TB] score case N=16 D=96 checksum=-31663104
[TB] attention case N=16 D=96 mismatch_count=0
[TB] attention case N=16 D=96 max_abs_error=0
[TB] attention case N=16 D=96 checksum=74785584
[TB] total_mismatch_count=0
[TB] PASS
INFO: [COSIM 212-1000] *** C/RTL co-simulation finished: PASS ***
```

这里可以明显看到，去掉 GEMM 固定右移之后，GEMM、Conv、QKV 的 checksum 都变大了。这个变化是预期内的，因为输出从“右移后的缩放值”变成了“原始 INT32 累加值”。关键是 HLS 输出和 C++/Python baseline 仍然逐元素一致，`mismatch_count=0`。

## 综合结果

| Top | BRAM_18K | DSP | FF | LUT | Estimated clock | RTL latency |
| --- | --- | --- | --- | --- | --- | --- |
| `gemm_top` | 2 | 16 | 3914 | 5226 | 7.142 ns | 544 cycles |
| `conv_top` | 15 | 36 | 47464 | 37641 | 7.272 ns | 15448 cycles |
| `qkv_top` | 2 | 16 | 3921 | 5375 | 7.300 ns | 360362 cycles |
| `attention_score_top` | 10 | 17 | 4517 | 5764 | 7.050 ns | max 23025 cycles |
| `attention_no_softmax_top` | 37 | 49 | 13135 | 18117 | 7.300 ns | max 407139 cycles |
| `attention_top` | 37 | 49 | 15884 | 20155 | 7.300 ns | max 408064 cycles |

## 我对资源的理解

这次扩容后，`GEMM_TILE` 没变，所以局部 4x4 MAC 阵列的基本 DSP 数量没有因为 tile 扩大而翻倍。但 `GEMM_MAX_K/M` 变大以后，片上数组和循环上限变大，像 Conv 和 Attention 这种 wrapper/core 里的中间数组也会明显影响 BRAM、LUT、FF 和 latency。

Conv 的 LUT/FF 比较高，我觉得主要和固定大数组初始化、im2col、weight flatten、reshape 这些外围逻辑有关。Attention 的 BRAM 和 DSP 更多，是因为 QKV、Score、P/V 路径里有多次 GEMM 和更多中间 buffer。

这次去掉 GEMM core 里的固定右移，对 GEMM 本体资源影响很小；真正资源变化主要体现在 Attention 里面的量化、row-normalization 和多级 buffer。`attention_top` 里的 row-normalization 仍然会生成整数除法器，这是后面要优化的点。

## 这一版的问题和后续想法

这一版已经能比较好地回应老师任务一：

```text
CNN: 一个固定卷积层 -> GEMM
Transformer: QKV + QK^T + P/S x V
GEMM: INT8 tiled 核心
验证: C-sim + C-synth + C/RTL cosim
```

后面我不准备继续盲目加大尺寸。下一步更应该做任务 2：

```text
设计 32/64-bit micro-instruction
写 accelerator_top()
用指令描述 GEMM / CONV2D / QKV / ATTN
由 accelerator_top 译码并调用这些已经验证过的 core
```

如果后面再扩大 CNN，可以考虑 `GEMM_MAX_N=64`，但那应该建立在当前 `16/96/96` 版本稳定、指令控制器也跑通之后。
