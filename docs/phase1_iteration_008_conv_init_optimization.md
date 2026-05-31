# Phase 1 / 迭代日志 008：去掉 Conv2D 里的大矩阵初始化

## 我这一版想解决什么

前面把 GEMM 最大尺寸扩到 `16/96/96` 以后，Conv2D 也从小测试扩到了：

```text
Input:  [3,6,6]
Weight: [4,3,3,3]
GEMM:   A[16,27] x B[27,4]
```

功能是通的，但我看报告时发现 `conv_top` 的 RTL latency 到了 `15448 cycles`。这个数比我直觉里一个小卷积层高很多，所以我开始怀疑是不是 Conv wrapper 里有很多不是计算本身的开销。

回头看 `conv2d_gemm()` 后，我发现自己之前写了三个初始化循环：

```text
A[GEMM_MAX_N][GEMM_MAX_K] 全部清 0
B[GEMM_MAX_K][GEMM_MAX_M] 全部清 0
C[GEMM_MAX_N][GEMM_MAX_M] 全部清 0
```

可是当前 Conv 实际只用：

```text
A[16,27]
B[27,4]
C[16,4]
```

也就是说，我为了固定接口保留了最大数组形状，但运行时不应该真的把最大数组区域都扫一遍。

## 我为什么觉得可以删

我重新按数据流检查了一遍：

```text
im2col
  -> 完整写入 active A[16,27]

weight flatten
  -> 完整写入 active B[27,4]

gemm_tiled(A, B, C, 16, 27, 4, true)
  -> 只读取 active A/B
  -> 完整写回 active C[16,4]

reshape
  -> 只读取 active C[16,4]
```

所以 full-matrix 初始化不是保证正确性的必要步骤，尤其是 `C` 的初始化更没有必要，因为 active `C` 会被 GEMM 结果完全覆盖。

我这里要注意的点是：不是所有初始化都可以随便删。只有当我能确认 active 区域会被完整写入，并且后续不会读到 inactive 区域时，删除才是安全的。如果以后做 padding、动态 shape 或者外部 tile，就要重新检查 active 区域是不是仍然完整赋值。

## 我改了哪些地方

| 文件 | 改动 |
| --- | --- |
| `hls/src/conv_core.cpp` | 删除 `A/B/C` 的 full-matrix 清零循环；后来又改成 1D flat 输入输出和自增地址。 |
| `hls/src/conv_types.h` | 增加 input/weight/output 的 flat size 和 stride 常量。 |
| `hls/src/conv_core.h` / `hls/src/conv_top.h` / `hls/src/conv_top.cpp` | Conv 外部接口从多维数组改成 1D flat 数组。 |
| `hls/tb/tb_conv.cpp` | testbench 也改成 flat input/weight/output，并保留多维打印和 golden reference。 |
| `README_conv.md` | 补充 Conv 初始化优化说明和最新验证结果。 |
| `README.md` | 更新 `conv_top` 的综合和 cosim 摘要。 |
| `docs/phase1_iteration_008_conv_init_optimization.md` | 记录这次定位和优化过程。 |

代码里仍然保留：

```cpp
static gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K];
static gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M];
static gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M];
```

这是因为 `gemm_tiled()` 的接口需要固定最大数组形状。当前优化先不改 GEMM 接口，只去掉明显无效的运行时清零。

## 问题 2：地址逻辑把 LUT/FF/DSP 拉得太高

删掉初始化以后，Conv latency 从 `15448 cycles` 降到了 `3154 cycles`，但资源还是很吓人：

```text
DSP = 36
FF  = 47329
LUT = 37207
```

这里尤其奇怪的是 DSP。我的 GEMM 微核是 4x4 MAC，理论上主要应该是 16 个 DSP，但 Conv top 里多出来了 20 个 DSP。我把这个现象问了 AI，也结合 HLS 日志重新看了一下，暂时理解是：问题主要来自多维数组、强行流水和循环拍平、非 2 的幂维度，以及复杂外围地址计算。特别是 `3x3x3=27` 这种维度被 HLS 拍平成流水线以后，硬件为了从一个线性 counter 反解 `ci/kh/kw`，容易生成 `urem/div`；另外多维地址表达式也可能被综合成额外乘法器。

之前综合日志里确实出现过类似这些模块：

```text
mul_64ns_66ns_129_5_1
urem_61ns_3ns_61_65_1
urem_62ns_4ns_62_66_1
urem_63ns_3ns_2_67_1
urem_64s_3ns_2_68_1
urem_64s_4ns_3_68_1
```

所以我这一版继续改 Conv wrapper，而不是动 GEMM core。

## 1D flatten + 增量寻址

我把 Conv 的外部接口从多维数组改成 1D：

```text
input[CONV_CIN][CONV_IN_H][CONV_IN_W]
  -> input[108]

weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW]
  -> weight[108]

output[CONV_COUT][CONV_OUT_H][CONV_OUT_W]
  -> output[64]
```

只改成 1D 还不够。如果代码里继续大量写：

```cpp
ci * stride + kh * stride + kw
```

HLS 还是可能生成复杂地址逻辑。所以我同时把核心搬运逻辑改成自增地址：

```text
input_ptr  按窗口位置自增
weight_ptr 按 kernel layout 自增
output_ptr 按输出 layout 自增
```

为了不让代码完全变成一堆魔数，我在 `conv_types.h` 里补了 stride 常量：

```cpp
CONV_INPUT_C_STRIDE
CONV_INPUT_H_STRIDE
CONV_WEIGHT_CO_STRIDE
CONV_WEIGHT_CI_STRIDE
CONV_WEIGHT_KH_STRIDE
CONV_OUTPUT_C_STRIDE
CONV_OUTPUT_H_STRIDE
```

同时在 `conv_core.cpp` 的 im2col 和 weight flatten 上方保留原来的多维数学布局注释。这个版本可读性确实比多维数组差一点，但换来的资源下降非常明显。

## 验证过程

我重新跑了 Conv 的完整 HLS 脚本：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_conv.tcl
```

脚本依次执行：

```text
csim_design
csynth_design
cosim_design -rtl verilog
```

## 终端关键结果

C-sim 仍然通过：

```text
[TB] Conv shape: input[3,6,6] flat[108], weight[4,3,3,3] flat[108], GEMM A[16,27] x B[27,4]
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=-72952
[PASS] Conv2D -> GEMM validation passed.
```

C/RTL cosim 也通过：

```text
INFO: [COSIM 212-1000] *** C/RTL co-simulation finished: PASS ***
```

cosim 报告：

```text
|   Verilog|      Pass|           2594|           2594|           2594|
```

## 综合结果对比

| 版本 | BRAM_18K | DSP | FF | LUT | Estimated clock | RTL latency |
| --- | --- | --- | --- | --- | --- | --- |
| 删除初始化前 | 15 | 36 | 47464 | 37641 | 7.272 ns | 15448 cycles |
| 删除初始化后 | 15 | 36 | 47329 | 37207 | 7.272 ns | 3154 cycles |
| 1D flatten + 自增地址后 | 15 | 16 | 1894 | 3574 | 7.103 ns | 2594 cycles |

第一步删初始化，主要降低 latency；第二步 1D flatten + 自增地址，主要降低 LUT/FF/DSP。BRAM 仍然是 15，没有变化，因为 `A/B/C` 和 GEMM 内部 buffer 的存储结构还没改。

我重新查生成 RTL 后，没有再看到之前那串 `urem_*` 和额外 `mul_64ns_*` 模块。现在 `conv_top` 的 DSP 数量回到 16，基本就是 GEMM 4x4 MAC 阵列本身。

我觉得这个结果挺有帮助：有时候资源没有明显变少，但 latency 会因为少扫了很多无用存储空间而大幅下降；而 LUT/FF/DSP 爆炸时，则要怀疑地址计算、循环拍平和非 2 的幂维度。

## 我学到的东西

这次我主要学到三点：

1. HLS 里 `static` 大数组保留的是硬件存储结构，不代表每次函数运行都必须把所有元素清一遍。
2. `GEMM_MAX_N/K/M` 是综合时的最大容量，实际调用的 `N/K/M` 才决定 active 计算区域。
3. 优化前要先看数据流，确认哪些数组元素真的会被读。盲目初始化最大数组虽然看起来稳，但可能会让 latency 非常难看。
4. HLS 里多维数组本身不是唯一问题，真正危险的是多维寻址、非 2 的幂循环、自动 loop flatten 和 pipeline 叠在一起，可能生成很重的 `urem/div/mul` 地址逻辑。
5. 1D flatten 会降低代码可读性，所以必须配合 stride 常量和布局注释，不然很容易把地址写错。

## 这一版的问题和后续想法

当前优化已经把 Conv 外围地址逻辑压下来了，但 `conv2d_gemm()` 里仍然有最大尺寸的 `A/B/C` 中间矩阵。后续如果继续优化 Conv，我觉得可以考虑：

```text
1. 只为 Conv active shape 准备更小的局部 A/B/C buffer。
2. 或者把 im2col 和 GEMM 的读取过程结合起来，减少完整 A 矩阵的落地。
3. 再往后才考虑更复杂的 padding、bias、ReLU、batch 或更大输入。
```

但这一步我先不继续动 GEMM 接口，因为当前目标是把已经验证过的 GEMM 作为稳定计算单元，先把 Conv wrapper 里明显不合理的 latency 浪费去掉。
