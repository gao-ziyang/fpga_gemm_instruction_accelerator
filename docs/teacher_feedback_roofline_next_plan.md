# 宫老师反馈后的理解：从功能验证转向资源-带宽-并行度权衡

## 1. 宫老师这段话我怎么理解

宫老师说：

```text
这个设计，其实就是要在计算并行度、片上缓存容量、片外访存带宽三者间，进行权衡。
测试的输入数据要大一些，在板载 DDR 能放下就行。
手动完成数据的划分，以及划分后数据 -> 片上缓存 -> 计算阵列的映射。
```

我现在理解下来，老师不是只想看一个小矩阵 GEMM 跑通，也不是只想看 `TILE=14` 把 DSP 用到 197 个。他更想看我能不能把 FPGA 加速器的基本设计问题讲清楚：

```text
1. 计算并行度：
   我实例化多少 MAC / DSP？每周期理论上能做多少乘加？

2. 片上缓存容量：
   A/B/C tile 能不能放进 BRAM？
   A/B/C buffer 怎么划分？
   哪些数据应该复用，哪些数据需要重新搬？

3. 片外访存带宽：
   DDR 到片上缓存每周期能搬多少数据？
   如果计算阵列很大，DDR/BRAM 能不能持续喂满？
   如果喂不满，DSP 就会空转。
```

所以后续工作重点应该从：

```text
小矩阵功能正确
```

升级为：

```text
更大输入规模 + 明确分块策略 + 片上缓存映射 + 性能/带宽分析
```

换句话说，我要做的不是“随便调大参数”，而是手动设计一套数据流：

```text
大矩阵在 DDR 中
  -> 手动切成 A_tile / B_tile / C_tile
  -> DMA/AXI 或 HLS 模拟接口搬到片上 A_buf/B_buf/C_buf
  -> A_buf/B_buf 按 bank/local buffer 映射到 MAC 阵列
  -> 计算阵列完成 tile GEMM
  -> C_tile 写回 DDR
```

当前 HLS 单元验证还没真正接 DDR，但也可以先在 C-sim/Cosim 中把这套分块和映射关系写清楚。

## 2. Zhang FPGA'15 CNN accelerator 论文给我的启发

论文：Chen Zhang 等，"Optimizing FPGA-based Accelerator Design for Deep Convolutional Neural Networks"，FPGA 2015。

这篇论文的核心观点和宫老师说的话非常一致：FPGA CNN accelerator 的设计不能只优化计算引擎，还必须同时分析计算吞吐和所需存储带宽。论文使用 roofline model，把一个设计点放到：

```text
x 轴：computation-to-communication ratio，也就是每访问一字节 DRAM 能做多少计算
y 轴：computational performance，也就是计算吞吐
```

roofline 的基本判断是：

```text
可达到性能 = min(计算资源上限, CTC ratio * 外存带宽)
```

这句话对我现在的 GEMM 很重要。`TILE=14` 时我已经接近把 DSP 用满，但实际吞吐没有接近理论值，说明设计已经不是简单的计算资源不足，而是数据供给、分块和边界利用率没有跟上。

论文还提出了几个和我当前任务直接相关的点。

### 2.1 Loop tiling 是必须的

CNN 卷积层原始循环大致是：

```text
row, col, output channel, input channel, kernel row, kernel col
```

论文把其中的 `row/col/to/ti` 做 tiling，对应 tile 参数：

```text
Tr, Tc, Tm, Tn
```

我当前 GEMM 里其实也在做类似的事情：

```text
N/M/K 维被 GEMM_TILE 和 GEMM_BLOCK_M 分块
```

只是我的分块目前还比较简单：

```text
N 方向：按 GEMM_TILE
M 方向：先按 GEMM_BLOCK_M，再按 GEMM_TILE
K 方向：按 GEMM_TILE 累加
```

后续如果测试更大矩阵，就必须像论文说的那样，手动说明每个 tile 的含义，而不是只说“我把矩阵变大了”。

### 2.2 计算并行度不是越大越好

论文里用 `Tm` 和 `Tn` 表示输出通道和输入通道方向的展开。它们决定计算引擎的并行规模，类似我现在的：

```text
GEMM_TILE * GEMM_TILE
```

但是论文也强调，展开因子受到 PE 数量、BRAM 容量和带宽约束。我的实验已经看到同样现象：

```text
TILE=4 -> 8：
  DSP 16 -> 64，latency 明显下降。

TILE=8 -> 12/14：
  DSP 64 -> 144/197，但 latency 基本不再下降。
```

原因不是 DSP 没加上去，而是片上缓存和供数结构没有同步升级。

### 2.3 Local memory promotion / data reuse 很关键

论文重点讨论了把可复用数据提升到片上 buffer，减少外部 DRAM 访问。对于 CNN 来说，可以复用：

```text
input feature map
weights
partial output
```

对 GEMM 来说，对应就是：

```text
A tile
B tile
C partial sum
```

我现在的 `A_bram/B_bram/localA/localB/localC` 就是第一版片上缓存结构，但还不够好：

```text
A_bram/B_bram -> localA/localB 仍然是串行加载
load 和 compute 还没重叠
B_bram 的 BLOCK_M 还比较小
没有系统分析 DDR/BRAM 需要的带宽
```

### 2.4 多层网络需要统一硬件参数

论文里提到：每一层 CNN 的最优 tiling/unroll 因子可能不同，但如果每层都换硬件结构很麻烦，所以他们选择一组跨层统一的 unroll factor，性能只比逐层最优略差。

这对我也有启发。老师任务里同时有 CNN 和 Transformer，所以我不能只为某一个小 case 选参数。更合理的说法是：

```text
我先选一组统一 GEMM 微核参数，让 Conv2D、QKV、QK^T、SxV 都能复用；
再用 benchmark 分析这组参数在不同 shape 下的利用率。
```

## 3. gemm_hls_ref 工程给我的启发

我 clone 的 `spcl/gemm_hls` 是高性能 GEMM 参考工程。它和我的工程不一样：

```text
gemm_hls_ref:
  面向 Alveo/VCU1525 大 FPGA
  使用 Vitis/OpenCL runtime
  top 是 m_axi + DATAFLOW
  内部是 stream + systolic processing elements
  追求高吞吐矩阵乘

我的工程:
  面向 ZYNQ7020
  当前主要做 HLS 单元验证
  top 还是 ap_memory
  后续才做 AXI/DMA/accelerator_top
```

但是它给了我一个非常清晰的对照：

```text
我的 GEMM：
  load_local_a -> load_local_b -> dot_k -> write_c
  阶段之间基本串行

gemm_hls_ref:
  ReadA / ReadB / Convert / Feed / PE / WriteC
  用 DATAFLOW + Stream 串起来
  PE 内部还有 double buffering
```

这解释了我现在的性能 gap。我的 `TILE` 变大后，计算阵列变大了，但供数通路还是阶段式的。参考工程则把数据流做成持续流动的 systolic pipeline。

我不应该直接搬它的代码，因为它太大，也面向不同平台；但可以学习它的设计思想：

```text
1. 用 DATAFLOW 拆开读数据、计算、写数据。
2. 用 stream 连接模块，减少阶段之间的硬等待。
3. 用 double buffering 遮挡数据加载。
4. 用 memory pack / compute pack 区分外部总线宽度和内部计算宽度。
5. 用理论性能公式和 expected runtime 区分理想计算性能与实际数据流性能。
```

## 4. 结合我当前实验，重新解释 TILE sweep 的结论

我当前固定：

```text
N = 16
K = 96
M = 96
```

这个 shape 是为了对应 Transformer QKV：

```text
X[16,96] x W[96,96]
```

这不是随便选的小矩阵，但它也有局限：`N=16` 对 `TILE=12/14` 不够友好，会产生边界空转。

现有结果：

```text
TILE=4:  DSP=16,  latency=118720
TILE=8:  DSP=64,  latency=54784
TILE=12: DSP=144, latency=52972
TILE=14: DSP=197, latency=54492
```

现在我更准确地理解为：

```text
TILE=4 -> 8:
  原来 MAC 阵列偏小，计算并行度不足；
  扩到 64 路 MAC 后，计算瓶颈明显缓解。

TILE=8 -> 12/14:
  MAC 阵列继续变大，但数据供给没有同步变强；
  load_local_a/b、B 分块搬运、边界空转成为瓶颈；
  所以 DSP 增长没有转化成 latency 下降。
```

这正好对应宫老师说的三者权衡：

```text
计算并行度：
  TILE 增大，DSP 增加。

片上缓存容量：
  B_bram 仍只按 BLOCK_M 缓存一部分 M；
  localA/localB 变大，但加载方式没有并行化。

片外访存带宽：
  当前 HLS 单元验证还没真正接 DDR，但未来大输入上板时，
  DDR -> AXI/DMA -> BRAM 的带宽一定会限制能否喂满 MAC 阵列。
```

## 5. 现在不应该马上做什么

我现在不应该直接做这些事：

```text
1. 不应该盲目把 GEMM_MAX_N/K/M 改到 256 或更大。
   因为这只会让数组、仿真和综合变重，但不一定解释瓶颈。

2. 不应该直接把 GEMM_BLOCK_M 拉到 96 当作最终方案。
   因为这会增加 BRAM/地址逻辑/控制复杂度，而且 Conv/Attention 还要资源。

3. 不应该直接搬 gemm_hls_ref 的 systolic array。
   因为平台、接口、工程复杂度都不同。

4. 不应该继续只跑很小的 16x96x96 后就下结论。
   因为大 TILE 在 N=16 下边界空转明显。
```

更合理的是分阶段验证每个判断。

## 6. 下一步建议路线

### Step 1：补一组更大的 GEMM benchmark

老师明确说输入数据要大一些，在板载 DDR 能放下就行。因此下一步先不急着上板，但 HLS benchmark 的矩阵要更像“DDR 中的大矩阵被分块处理”。

建议第一组：

```text
N = 64
K = 96
M = 96
```

原因：

```text
1. K/M 仍保持 96，和 Transformer 隐藏维度一致。
2. N 从 16 扩到 64，减少 TILE=12/14 的边界空转影响。
3. 数据量仍然很小，DDR 肯定放得下。
4. 总 MAC = 64*96*96 = 589824，是原来的 4 倍，更适合看稳定吞吐。
```

如果资源和仿真时间还能接受，再测：

```text
N = 96
K = 96
M = 96
```

这样对 `TILE=12` 是完全整除的，对分析更干净。

### Step 2：手动写清楚分块映射

对大矩阵，不只调用：

```cpp
gemm_tiled(A, B, C, N, K, M, true);
```

还要在文档和代码中明确：

```text
DDR 里的 A[N,K] 如何切 A_tile
DDR 里的 B[K,M] 如何切 B_tile
C[N,M] 的 partial sum 如何累加
A_tile 放到哪块 A_bram
B_tile 放到哪块 B_bram
localA/localB 怎么从 BRAM 取数
MAC 阵列每次算哪个 C_tile
```

这一点就是老师说的“手动完成数据的划分，以及划分后数据 -> 片上缓存 -> 计算阵列的映射”。

### Step 3：做 GEMM_BLOCK_M sweep

当前：

```text
BLOCK_M = 8/12/14
```

下一步建议固定 `TILE=8` 或 `TILE=12`，测试：

```text
BLOCK_M = 8
BLOCK_M = 16
BLOCK_M = 32
```

暂时不建议一开始就 `BLOCK_M=96`。原因：

```text
BLOCK_M 变大可以减少 B 分块次数；
但也会扩大 B_bram，增加地址和控制逻辑；
最后系统里 Conv/Attention/accelerator_top 还要留资源。
```

这个实验能回答一个关键问题：

```text
当前 latency 卡住，到底有多少来自 B 分块搬运？
```

### Step 4：尝试 DATAFLOW 的低风险版本

先不做完整 systolic array。第一版只尝试：

```text
load_local_a 和 load_local_b 并行
```

因为它们分别读 `A_bram` 和 `B_bram`，写 `localA` 和 `localB`，天然没有强数据依赖。

目标是把：

```text
load_local_a + load_local_b + compute
```

变成近似：

```text
max(load_local_a, load_local_b) + compute
```

这一步风险比完整 double buffering 小，适合先做。

### Step 5：再考虑 double buffering

如果 Step 4 有收益，再尝试：

```text
计算当前 k tile
同时加载下一组 localA/localB
```

但我现在也要清楚：如果 load 远慢于 compute，double buffering 只能隐藏一部分等待，不能根治。真正要喂满大 MAC 阵列，还要提高 local load 带宽。

### Step 6：最后再考虑 BRAM banking

BRAM banking 才是提高 `BRAM -> local buffer` 带宽的关键，但它资源风险最大。

可能方向：

```text
A_bram 按 N/i 维 cyclic partition
B_bram 按 M/j 维 cyclic partition
load_local_a/b 里适当 UNROLL
```

但这可能增加 BRAM bank、LUT、mux 和布线压力，所以应该放在更后面。

## 7. 可以向宫老师这样汇报

我可以这样组织回复：

```text
老师，我理解这个任务现在核心不是单纯把 GEMM 跑通，而是分析计算并行度、片上缓存和片外带宽之间的平衡。我前面固定 N=16,K=96,M=96 做了 TILE=4/8/12/14 的 sweep，DSP 从 16 增加到 197，接近把 7020 的 DSP 用满。结果显示 TILE=4 到 8 latency 下降明显，但继续增大到 12/14 后提升很小，说明瓶颈已经从 MAC 数量转移到数据供给和分块调度。

结合您推荐的 FPGA'15 CNN accelerator 论文，我现在理解应该用类似 roofline 的思路，把每个设计点的计算吞吐和所需外存带宽一起分析。后面我准备把测试输入变大，例如先做 N=64,K=96,M=96 或 N=96,K=96,M=96；同时手动写清楚大矩阵如何划分成 tile、tile 如何搬到 A/B 片上缓存、缓存如何映射到 MAC 阵列。之后再分别测试 BLOCK_M、DATAFLOW 和可能的 banking，定位是 B 分块、local buffer 加载还是边界空转造成的 gap。
```

这段话的重点是：

```text
我不是只调参数；
我是用论文里的计算-访存平衡思想来设计下一步实验。
```

## 8. 我现在的结论

当前阶段可以总结为：

```text
功能层面：
  GEMM / Conv2D via GEMM / QKV / Attention 已经跑通。

性能层面：
  TILE sweep 已证明 DSP 可以扩到接近用满；
  但实际吞吐没线性提升，瓶颈来自数据供给和分块映射。

下一步：
  按论文的 roofline/design-space exploration 思路，
  用更大输入和更明确的数据划分来分析计算并行度、片上缓存、片外带宽三者的权衡。
```

