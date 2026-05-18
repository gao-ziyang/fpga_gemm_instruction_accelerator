# 迭代日志 006：补 Transformer Attention 的核心矩阵流

## 我这一版想解决什么

前面 QKV projection 已经能跑通了，但它只覆盖了：

```text
Q = X * Wq
K = X * Wk
V = X * Wv
```

这一版我继续往 Transformer Attention 的核心路径推进：

```text
Score = Q * K^T
Out = P * V
```

我没有一上来做完整 Encoder、多头、多层或者严格 softmax，而是先分阶段做：

```text
1. 单独验证 QK^T
2. 加 Q/K/V 的 int8 量化
3. 做 no-softmax 版本：Out_raw = Score_q * V_q
4. 做 row-normalization 近似版本：Out = P_q * V_q
```

这样每一步都能用 C++ reference 对齐，不至于一下子把问题堆在一起。

## 我改了哪些文件

| 文件 | 改动 |
| --- | --- |
| `hls/src/attention_core.h` | 声明 attention 相关 core 函数。 |
| `hls/src/attention_core.cpp` | 实现量化、K 转置、QK^T、no-softmax、row-normalization attention。 |
| `hls/src/attention_top.cpp` | 新增 `attention_score_top`、`attention_no_softmax_top`、`attention_top` 三个验证入口。 |
| `hls/tb/tb_attention.cpp` | 新增 C++ reference，比较 score、no-softmax、row-normalization 的输出。 |
| `hls/scripts/run_hls_attention_score.tcl` | 验证 `attention_score_top`。 |
| `hls/scripts/run_hls_attention_no_softmax.tcl` | 验证 `attention_no_softmax_top`。 |
| `hls/scripts/run_attention_hls.tcl` | 验证最终 `attention_top`。 |
| `README_attention.md` | 单独整理 Transformer attention 模块。 |

这里依然保持一个原则：`attention_top()` 只是验证入口，后续真正给 `accelerator_top()` 调用的应该是 `attention_core()` 或更细的 core 函数。

## 我学到的东西

### 1. Q/K/V 不能直接继续喂给 GEMM

`qkv_projection()` 输出的是 `gemm_acc_t`，也就是 `ap_int<32>`。但是我的 `gemm_tiled()` 输入是 `gemm_data_t`，也就是 `ap_int<8>`。

在迭代 007 之后，`gemm_tiled()` 自己只输出原始 INT32 累加值，不再固定右移。所以 Q/K/V 的 INT32 结果更明确了：它们是 projection 的原始累加输出，只有后面继续做 `QK^T` 时才需要重新量化成 INT8。

所以我必须加一个量化函数：

```cpp
gemm_data_t saturate_to_int8(gemm_acc_t x, int shift);
```

它做的是：

```text
右移 shift -> 超过 127 截断 -> 小于 -128 截断
```

这样得到的 `Q_q/K_q/V_q` 才能继续复用 `gemm_tiled()`。

### 2. K 转置其实是为了把 QK^T 变成 GEMM

Score 的数学式是：

```text
Score[i][j] = sum_d Q[i][d] * K[j][d]
```

要用 GEMM 来做，就需要把 K 变成：

```text
K_T[d][j] = K[j][d]
```

然后调用：

```cpp
gemm_tiled(Q_q, K_T, Score, N, D, N, true);
```

### 3. no-softmax 是一个很好的中间检查点

如果直接做 `softmax(QK^T)V`，出了错不容易定位。所以我先做：

```text
Score = Q_q * K_q^T
Score_q = quantize(Score)
Out_raw = Score_q * V_q
```

这不是完整 attention，但它能验证完整数据流：

```text
QKV -> QK^T -> SxV
```

### 4. row-normalization 只是第一版近似

我现在的 `attention_top()` 里没有做严格 exp softmax，而是：

```text
P[i][j] = max(Score[i][j], 0)
row_sum = sum_j P[i][j]
P_q[i][j] = (P[i][j] << P_SHIFT) / row_sum
```

这个版本能综合、能 cosim，但综合报告里出现了 `sdiv` 除法器。我的理解是：这一版的价值主要是把流程跑通，后续如果老师问资源优化，就可以说 row-normalization 里的除法要继续用 LUT、倒数近似或更简单的归一化替代。

## 验证过程

运行命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_score.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_no_softmax.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_attention_hls.tcl
```

testbench 跑了三个尺寸：

```text
N=4,  D=4
N=8,  D=8
N=16, D=96
```

最终 C-sim 关键输出：

```text
[TB] score case N=16 D=96 mismatch_count=0
[TB] score case N=16 D=96 max_abs_error=0
[TB] score case N=16 D=96 checksum=-31663104
[TB] attention case N=16 D=96 mismatch_count=0
[TB] attention case N=16 D=96 max_abs_error=0
[TB] attention case N=16 D=96 checksum=74785584
[TB] total_mismatch_count=0
[TB] PASS
```

C/RTL cosim：

| Top | 结果 | Max latency |
| --- | --- | --- |
| `attention_score_top` | PASS | 23025 cycles |
| `attention_no_softmax_top` | PASS | 407139 cycles |
| `attention_top` | PASS | 408064 cycles |

C-synthesis 资源摘要：

| Top | BRAM_18K | DSP | FF | LUT | Estimated clock |
| --- | --- | --- | --- | --- | --- |
| `attention_score_top` | 10 | 17 | 4517 | 5764 | 7.050 ns |
| `attention_no_softmax_top` | 37 | 49 | 13135 | 18117 | 7.300 ns |
| `attention_top` | 37 | 49 | 15884 | 20155 | 7.300 ns |

## 过程中看到的 warning

综合 `attention_top` 时会有一些：

```text
Ignore interface attribute or pragma which is not used in top function
```

这是因为 `attention_top.cpp` 里同时放了 `attention_score_top`、`attention_no_softmax_top`、`attention_top` 三个验证入口。当前脚本只把其中一个设成 top，其他函数里的接口 pragma 自然会被忽略。我现在把它理解为“多验证入口放在同一文件带来的提示”，不是功能错误。

## 这一版的问题和后续想法

这一版已经能说明 Transformer Attention 的核心矩阵乘流程可以复用 GEMM：

```text
QKV projection
QK^T
S/P x V
```

但它还不是完整 Encoder：

```text
没有多头
没有多层
没有 LayerNorm
没有 FFN
softmax 只是 row-normalization 近似
```

我后续更想先进入任务 2，把已经验证过的 GEMM / Conv / QKV / Attention core 用一套最小 micro-instruction 串起来。这样比继续堆复杂网络更贴近老师说的“用指令描述网络层并映射到计算单元”。
