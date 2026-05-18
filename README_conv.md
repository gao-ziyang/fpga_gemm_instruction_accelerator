# Conv2D -> GEMM 验证说明

这个模块验证一件事：CNN 里的一个 Conv2D 层可以先做 lowering/im2col，再复用已经验证过的 `gemm_tiled()` 完成计算。

## 当前卷积层参数

| 参数 | 数值 |
| --- | --- |
| `CONV_CIN` | 3 |
| `CONV_IN_H` | 6 |
| `CONV_IN_W` | 6 |
| `CONV_KH` | 3 |
| `CONV_KW` | 3 |
| `CONV_COUT` | 4 |
| `CONV_STRIDE` | 1 |
| Padding | 0 |
| Bias / ReLU | 暂无 |
| `CONV_OUT_H` | 4 |
| `CONV_OUT_W` | 4 |

数学公式：

```text
output[co][oh][ow] =
    sum_{ci,kh,kw}
        input[ci][oh + kh][ow + kw] * weight[co][ci][kh][kw]
```

## GEMM 映射

```text
A = im2col(input)
  shape = [Ho * Wo, Cin * Kh * Kw]
  shape = [16, 27]

B = flattened weight
  shape = [Cin * Kh * Kw, Cout]
  shape = [27, 4]

C = output matrix
  shape = [Ho * Wo, Cout]
  shape = [16, 4]
```

也就是：

```text
C[16,4] = A[16,27] x B[27,4]
```

当前调用：

```cpp
gemm_tiled(A, B, C, CONV_GEMM_N, CONV_GEMM_K, CONV_GEMM_M, true);
```

当前 `gemm_tiled()` 输出的是原始 INT32 累加结果，不再在 GEMM core 里固定右移。也就是说这个 Conv2D baseline 直接按卷积公式求 raw sum，然后和 HLS 输出逐元素比较。如果后面要把这个卷积输出继续作为下一层 INT8 输入，再单独加可配置的 requantization。

## Top 和 Core 的分工

我现在固定一条规则：

```text
conv_top()
  -> 只作为 HLS 单元验证入口
  -> 放 ap_memory / ap_ctrl_hs 接口 pragma
  -> 调用 conv2d_gemm()

conv2d_gemm()
  -> 真正的 Conv2D -> im2col -> GEMM -> reshape 核心逻辑
  -> 后续 accelerator_top 应该调用这个函数
```

这样做是为了避免后面系统级顶层和单元测试顶层混在一起。

## 当前优化记录

Conv2D 这一版保留 `A/B/C` 为 `GEMM_MAX_*` 尺寸，是因为 `gemm_tiled()` 的函数接口需要固定最大数组形状；但实际本层只使用：

```text
A[16,27]
B[27,4]
C[16,4]
```

我之前在 `conv2d_gemm()` 里把 `A[GEMM_MAX_N][GEMM_MAX_K]`、`B[GEMM_MAX_K][GEMM_MAX_M]`、`C[GEMM_MAX_N][GEMM_MAX_M]` 全部清零，综合后发现 Conv latency 很高。重新看数据流后，我发现 active 区域里的 `A/B` 会被 im2col 和 weight flatten 完整写入，active 区域里的 `C` 会被 `gemm_tiled()` 完整写回，所以运行时没有必要清空整个最大矩阵。

因此当前版本删除了 Conv wrapper 里的 full-matrix 初始化，只保留真正需要的 im2col、weight flatten、GEMM 和 reshape。删除后 C-sim 和 C/RTL cosim 仍然逐元素一致。

随后我又把 `conv_top()` 的外部数组接口改成 1D flatten：

```text
input:  [3][6][6]       -> input[108]
weight: [4][3][3][3]    -> weight[108]
output: [4][4][4]       -> output[64]
```

同时在 `conv2d_gemm()` 里用 `input_ptr`、`weight_ptr`、`output_ptr` 这类自增地址代替多维公式寻址。这样可读性会差一点，所以我在 `conv_types.h` 里保留了 stride 常量，并在代码注释里保留原来的多维数学布局。这样做的收益是减少 HLS 为非 2 的幂维度和拍平循环生成的 `urem/div/mul` 地址逻辑。

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `hls/src/conv_types.h` | 固定 Conv2D 参数、flat size、stride 和 GEMM 映射尺寸。 |
| `hls/src/conv_core.h/.cpp` | `conv2d_gemm()`，用 1D input/weight/output 完成 im2col、weight flatten、GEMM 和 output reshape。 |
| `hls/src/conv_top.h/.cpp` | HLS 单元验证 top，接口为 flat `ap_memory` 数组。 |
| `hls/tb/tb_conv.cpp` | flat 输入输出的 direct convolution golden，对比 `conv_top()` 输出。 |
| `hls/scripts/run_hls_conv.tcl` | 自动运行 C-sim、C-synth、C/RTL cosim。 |

## 如何运行

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_conv.tcl
```

## 验证结果

C-sim 关键输出：

```text
[TB] Conv shape: input[3,6,6] flat[108], weight[4,3,3,3] flat[108], GEMM A[16,27] x B[27,4]
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=-72952
[PASS] Conv2D -> GEMM validation passed.
```

C/RTL cosim：

```text
Verilog: PASS
Latency: 2594 cycles
```

C-synthesis 摘要：

| Top | BRAM_18K | DSP | FF | LUT | Estimated clock | Latency |
| --- | --- | --- | --- | --- | --- | --- |
| `conv_top` | 15 | 16 | 1894 | 3574 | 7.103 ns | 2594 cycles |

和最初 full-matrix 初始化版本相比，RTL latency 从 `15448 cycles` 降到 `2594 cycles`。和只删除初始化的版本相比，1D flatten + 增量寻址主要把 `DSP 36 -> 16`、`LUT 37207 -> 3574`、`FF 47329 -> 1894`，说明原先很大一部分资源确实花在 Conv 外围地址逻辑上。

## 当前限制

当前只验证一个固定参数推理层，没有 bias、ReLU、padding、batch，也没有做 AXI/DMA 上板接口。后续如果继续扩展 CNN，可以先加 padding，再加 bias/ReLU，最后再考虑更大的输入特征图和外部 tiling。
