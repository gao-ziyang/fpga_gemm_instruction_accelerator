# Transformer Attention 验证说明

这个模块验证 Transformer 里最核心的矩阵计算路径：

```text
Q = X * Wq
K = X * Wk
V = X * Wv
Score = Q_q * K_q^T
Out = P_q * V_q
```

当前目标不是完整 Transformer Encoder，也不是训练，而是先证明 GEMM 计算单元能支撑 QKV、QK^T 和 S/P x V 这些关键推理计算。

这一版里 `gemm_tiled()` 本身只输出原始 INT32 累加结果，不再固定做 `>>8`。所以 Q/K/V projection 的输出保持 `gemm_acc_t`，只有在它们要继续作为下一个 GEMM 的 INT8 输入时，才通过 `saturate_to_int8(x, shift)` 做可配置右移和饱和。

## 为什么需要量化

`qkv_projection()` 的输出类型是 `gemm_acc_t`，也就是 `ap_int<32>`。但是 `gemm_tiled()` 的输入类型是 `gemm_data_t`，也就是 `ap_int<8>`。

所以 Q/K/V 不能直接继续喂给 GEMM，需要先做：

```cpp
gemm_data_t saturate_to_int8(gemm_acc_t x, int shift);
```

这个函数做三件事：

```text
1. 右移 shift
2. 大于 127 截断为 127
3. 小于 -128 截断为 -128
```

这样得到 `Q_q/K_q/V_q`，再继续做后面的 attention GEMM。

## 当前阶段

| 阶段 | Top | 功能 |
| --- | --- | --- |
| Attention score | `attention_score_top` | `Score = Q_q x K_q^T` |
| No-softmax attention | `attention_no_softmax_top` | `QKV -> Score -> Score_q x V_q` |
| Row-normalization attention | `attention_top` | `QKV -> Score -> P_q x V_q` |

当前 `row_normalize_score()` 不是严格 softmax，而是一个第一版硬件友好的近似：

```text
P[i][j] = max(Score[i][j], 0)
row_sum = sum_j P[i][j]
P_q[i][j] = (P[i][j] << ATTENTION_P_SHIFT) / row_sum
```

它的优点是容易在 HLS 里写通；缺点是综合后会出现整数除法器，后续还需要考虑 LUT、倒数近似或者更轻的归一化方法。

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `hls/src/attention_core.h/.cpp` | attention 核心函数：量化、K 转置、score、row-normalization、P/V GEMM。 |
| `hls/src/attention_top.cpp` | 三个 HLS 单元验证 top：score、no-softmax、row-normalization。 |
| `hls/tb/tb_attention.cpp` | C++ reference，对比 score、no-softmax 和 row-normalization 输出。 |
| `hls/scripts/run_hls_attention_score.tcl` | 验证 `attention_score_top`。 |
| `hls/scripts/run_hls_attention_no_softmax.tcl` | 验证 `attention_no_softmax_top`。 |
| `hls/scripts/run_attention_hls.tcl` | 验证最终 `attention_top`。 |

## 如何运行

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_score.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_no_softmax.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_attention_hls.tcl
```

## 验证尺寸

testbench 同时跑三个尺寸：

```text
N=4,  D=4
N=8,  D=8
N=16, D=96
```

其中 `N=16,D=96` 对应当前老师任务里比较像样的 Transformer 子模块规模：

```text
QKV:  [16,96] x [96,96]
QK^T: [16,96] x [96,16]
P*V:  [16,16] x [16,96]
```

当前验证里：

```text
q_shift = 8
score_shift = 8
p_shift = ATTENTION_P_SHIFT
```

这些 shift 都在 attention 后处理里显式使用，不在 GEMM core 里固定。

## 验证结果

C-sim 关键输出：

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

这里的 attention case 在 testbench 中同时比较了 no-softmax 输出和 row-normalization 输出，checksum 是两部分合并后的检查值。

C/RTL cosim：

| Top | Verilog | Max latency |
| --- | --- | --- |
| `attention_score_top` | PASS | 23025 cycles |
| `attention_no_softmax_top` | PASS | 407139 cycles |
| `attention_top` | PASS | 408064 cycles |

C-synthesis 摘要：

| Top | BRAM_18K | DSP | FF | LUT | Estimated clock |
| --- | --- | --- | --- | --- | --- |
| `attention_score_top` | 10 | 17 | 4517 | 5764 | 7.050 ns |
| `attention_no_softmax_top` | 37 | 49 | 13135 | 18117 | 7.300 ns |
| `attention_top` | 37 | 49 | 15884 | 20155 | 7.300 ns |

## 当前限制

1. `attention_top` 使用 row-normalization，不是严格 softmax。
2. 当前是单头、固定最大尺寸的推理子模块，不是多头多层完整 Encoder。
3. `*_top` 仍然是单元验证入口，最终系统应该由 `accelerator_top()` 调用 `attention_core()`。
4. 资源优化还没开始，尤其是 row-normalization 里的除法器后续需要重点处理。
