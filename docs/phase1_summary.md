# Phase 1 汇总：从小 GEMM 到早期算子验证

Phase 1 的主要目标不是追求最终性能，而是把 HLS 的基本方法和几个神经网络算子的形态跑通：先确认 INT8 GEMM 能算对，再把 Conv、QKV、Attention 拆成能映射到 GEMM 的形式。

## 这一阶段做了什么

最早的最小 GEMM 用很小的矩阵验证了 `ap_int`、局部数组、循环展开和 testbench。代表性结果是：

```text
4x4 GEMM
C-sim PASS
latency = 56 cycles
BRAM = 0
DSP  = 2
LUT  = 485
```

之后逐步加入 tiled GEMM、buffer、边界处理、量化，以及 QKV projection、Conv2D via GEMM、Attention 的功能验证。这些早期文件后来没有全部保留在 `hls/src`，因为当前主线已经收敛到 `gemm_scheduler`、`accelerator_instruction` 和 `accelerator_top_axi`。

## Conv 早期优化

这一阶段比较有价值的一组数据是 Conv 初始化和地址展开优化：

| 版本 | BRAM | DSP | FF | LUT | clock | RTL latency |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 删除初始化前 | 15 | 36 | 47464 | 37641 | 7.272 ns | 15448 |
| 删除初始化后 | 15 | 36 | 47329 | 37207 | 7.272 ns | 3154 |
| 1D flatten + 自增地址 | 15 | 16 | 1894 | 3574 | 7.103 ns | 2594 |

这里学到的东西很直接：HLS 里看似普通的数组清零、二维/三维索引和乘法地址计算，都会变成真实硬件。能用自增地址和扁平数组表达时，资源会明显下降。

## GEMM tile sweep

固定 `N=16, K=96, M=96` 时，总 MAC 为：

```text
16 * 96 * 96 = 147456
```

早期 sweep 结果：

| TILE | DSP | BRAM | FF | LUT | RTL latency | 理论吞吐 | 实测吞吐 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 16 | 2 | 1599 | 1552 | 118720 | 16.00 | 1.24 |
| 8 | 64 | 2 | 5448 | 3441 | 54784 | 64.00 | 2.69 |
| 12 | 144 | 2 | 11865 | 6824 | 52972 | 144.00 | 2.78 |
| 14 | 197 | 2 | 16271 | 9393 | 54492 | 196.00 | 2.71 |

这张表后来变得很重要：单纯把 MAC 阵列做大，早期 scheduler 没跟上时，完整 GEMM 的 MAC/cycle 并不会按 `TILE*TILE` 线性增长。

## 阶段结论

Phase 1 跑通了“算子能否用 HLS 表达并验证正确”的问题，但还没有形成板级可用的 AXI IP。早期的动态 top 也暴露出一个问题：当 `N/K/M` 不是固定常量时，C-synth report 不一定能给出完整固定 shape latency，后续才转向 fixed-shape benchmark、RTL cosim 和板级计时来确认性能。

这一阶段留下的核心经验是：

1. 功能验证要从小矩阵开始，否则 debug 成本太高。
2. HLS 资源主要被数组分区、循环展开、乘法地址计算和初始化逻辑影响。
3. GEMM 的真实性能不能只看 `TILE*TILE`，必须看 scheduler 的 load/compute/store 是否能喂饱阵列。
