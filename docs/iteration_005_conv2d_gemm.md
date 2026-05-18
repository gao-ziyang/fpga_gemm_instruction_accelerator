# 迭代日志 005：把 Conv2D 映射到 GEMM

## 我这一版想解决什么

前面我已经有了一个能跑通的 `gemm_tiled()`，所以这一版开始补 CNN 里的卷积层。我的想法很直接：先不做完整 CNN，也不重新设计一个专门的卷积阵列，而是把一层 Conv2D 变成 GEMM：

```text
Conv2D -> im2col/lowering -> GEMM -> reshape output
```

这样我就能向老师解释：CNN 里的一层卷积，本质上也可以落到矩阵乘计算单元上。

## 我是怎么一步一步做的

一开始我先做很小的版本：

```text
Cin=1, H=W=3, Kh=Kw=2, Cout=2
A[4,4] x B[4,2] -> C[4,2]
```

这个版本主要用来检查 im2col 的行列顺序、weight flatten 的顺序和 output reshape 有没有写反。

然后我把 `Cin` 加到 2：

```text
Cin=2, Cout=1
A[4,8] x B[8,1] -> C[4,1]
```

这一步让我确认多输入通道的累加是对的。这里最容易错的地方是 A 的列展开顺序必须和 B 的行展开顺序完全一致。我现在用的是：

```text
ci -> kh -> kw
```

后来再把 `Cout` 加到 2：

```text
Cin=2, Cout=2
A[4,8] x B[8,2] -> C[4,2]
```

在后面 GEMM 最大尺寸扩到 `16/96/96` 以后，我又把 Conv 版本扩到现在这个更像样一点的固定层：

```text
Input:  [3,6,6]
Weight: [4,3,3,3]
Output: [4,4,4]
GEMM:   A[16,27] x B[27,4] -> C[16,4]
```

这一版已经覆盖了多输入通道、多输出通道和 3x3 卷积核。

## 我改了哪些文件

| 文件 | 作用 |
| --- | --- |
| `hls/src/conv_types.h` | 固定 Conv2D 参数和对应 GEMM 尺寸。 |
| `hls/src/conv_core.h/.cpp` | 真正的 Conv2D -> im2col -> GEMM -> reshape 逻辑。 |
| `hls/src/conv_top.h/.cpp` | HLS 单元验证 top，只写接口 pragma 并调用 core。 |
| `hls/tb/tb_conv.cpp` | direct convolution golden，对比 HLS 输出。 |
| `hls/scripts/run_hls_conv.tcl` | 自动跑 C-sim、C-synth、C/RTL cosim。 |
| `README_conv.md` | 单独整理 Conv2D 模块。 |

这里我专门做了一个结构调整：`conv_top()` 只作为测试入口，`conv2d_gemm()` 才是以后真正要被 `accelerator_top()` 调用的核心函数。

我现在给自己定的规则是：

```text
*_top()
  -> 单元验证用
  -> 写 ap_memory / ap_ctrl_hs 接口
  -> 给 Vitis HLS 当 top function

*_core() 或具体核心函数
  -> 后续系统级 accelerator_top 调用
  -> 尽量不和上板接口绑死
```

这个规则感觉很重要，因为后面如果把所有小模块的 top 都塞进系统级控制里，接口会越来越乱。

## 我学到的东西

### 1. Conv2D 到 GEMM 的关系

卷积公式是：

```text
output[co][oh][ow] =
    sum input[ci][oh+kh][ow+kw] * weight[co][ci][kh][kw]
```

如果把每个输出位置的输入窗口展开成一行，就得到 A：

```text
A shape = [Ho*Wo, Cin*Kh*Kw]
```

如果把每个输出通道的卷积核展开成一列，就得到 B：

```text
B shape = [Cin*Kh*Kw, Cout]
```

最后：

```text
C = A x B
```

再把 C reshape 回 `output[co][oh][ow]`。

### 2. 后来我把 GEMM 的固定右移拿掉了

这一版刚开始做 Conv 的时候，我的 GEMM core 里还带着固定 `>> 8`，所以当时 direct convolution 的 golden 也跟着右移。后来在迭代 007 里我重新想了一下，觉得 GEMM core 还是应该只做原始矩阵乘：

```text
C = A x B
INT8 x INT8 -> INT32 accumulate
```

所以当前版本的 Conv golden 已经改成 raw conv sum，不再右移。后续如果卷积输出要继续作为下一层 INT8 输入，再单独加可配置的 shift + saturate。

### 3. 当前接口只是为了单元验证

`conv_top()` 里现在是：

```cpp
#pragma HLS INTERFACE ap_memory port=input
#pragma HLS INTERFACE ap_memory port=weight
#pragma HLS INTERFACE ap_memory port=output
#pragma HLS INTERFACE ap_ctrl_hs port=return
```

我的理解是：当前输入输出数组在 HLS 单元验证里被当成 memory-like 端口，`ap_ctrl_hs` 提供 start/done/idle/ready 这类函数级握手。这个写法适合 C-sim、C-synth、C/RTL cosim。

但这不是最终上板接口。后面真正上板时，接口应该集中到 `accelerator_top()` 里规划，比如 AXI-Lite 控制寄存器、AXI Master 或 DMA 读写 DDR。

### 4. 后续系统路径的理解

我之前还想过：如果最后是一个加速器，是不是可以电脑串口直接连 PL 顶层，把它想成类似一块插在电脑上的外设。后来我觉得这个理解需要修正。

更合理的路径应该是：

```text
PC 串口/上位机
  -> PS 端接收命令或数据
  -> PS 通过 AXI-Lite 配置 PL
  -> PL accelerator_top 通过 AXI Master/DMA 读写 DDR
  -> PL 算完后给 done
  -> PS 读回结果，再通过串口或其他方式返回 PC
```

也就是说，串口更适合作为 PC 和 PS 的调试入口；PL 主要做计算和数据通路。除非自己额外写 UART 接收/解析逻辑，否则不太建议让串口直接拉到 PL 计算顶层。

## 验证过程

运行命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_conv.tcl
```

脚本依次执行：

```text
csim_design
csynth_design
cosim_design -rtl verilog
```

最终版本的 C-sim 关键输出：

```text
[TB] Conv shape: input[3,6,6], weight[4,3,3,3], GEMM A[16,27] x B[27,4]
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=-72952
[PASS] Conv2D -> GEMM validation passed.
```

C/RTL cosim：

```text
Verilog: PASS
Latency: 15448 cycles
```

C-synthesis 摘要：

| 项目 | 结果 |
| --- | --- |
| Part | `xc7z020clg400-2` |
| Clock target | 10 ns |
| Estimated clock | 7.272 ns |
| BRAM_18K | 15 |
| DSP | 36 |
| FF | 47464 |
| LUT | 37641 |
| Latency | 15448 cycles |

## 这一版的问题和后续想法

这一版功能是通的，但资源不算漂亮，尤其是 Conv wrapper 里为了构造 A/B/C 的大数组和初始化循环，会带来不少 LUT/FF 和延迟。后续如果要优化，我觉得可以从两个方向继续：

```text
1. 保持当前清楚的 im2col 结构，先用于报告和验证。
2. 后面再尝试把 im2col 和 GEMM 的 A/B buffer 更紧地结合起来，减少中间大矩阵初始化和搬运。
```

目前我先不急着把 Conv 做复杂。对老师任务来说，这一版已经能说明“CNN 的一个卷积层可以映射到 GEMM 计算单元”，后面更重要的是把 Transformer attention 和任务 2 的指令控制器接上。
