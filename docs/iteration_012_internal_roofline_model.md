# Iteration 012：先做内部 roofline 建模，再继续优化 scheduler

## 我这一版想解决什么

前面 O0-O5 已经说明一个问题：我不能再靠“感觉加 pragma”。`TILE=14` 已经把 DSP 用到 196 个，但是实际 MAC/cycle 远低于 `14x14=196` 的理论值；O2 加 row banking 有收益，O4/O5 合并 local A/B helper 反而退化。这说明瓶颈不只是计算阵列，而是片上 buffer、banking、local feeding 和 HLS 地址/控制逻辑共同决定的。

所以这一版我先不继续乱改 HLS 核心，而是做一件更基础的事：把老师反馈和两篇论文的启发整理成一个可以反复使用的 roofline-style 分析模型。

## 两篇论文给我的启发

第一篇 DianNao 给我的启发是：计算阵列不是唯一核心，片上 buffer 也是架构本身的一部分。它把输入、权重和输出分别放进类似 NBin、SB、NBout 的结构，再喂给 NFU 计算单元。对应到我现在的工程，大概是：

```text
DianNao NBin  -> A_buf
DianNao SB    -> B_buf
DianNao NBout -> C_buf
DianNao NFU   -> gemm_core_mac
DianNao CP    -> instruction decode / scheduler
```

这让我重新看 O0-O5：`A_buf/B_buf/C_buf` 不是附属变量，而是决定性能和资源的核心结构。BRAM banking、端口数量、mux 和地址逻辑，可能比 DSP 数量更危险。

第二篇 FPGA'15 CNN accelerator 论文给我的启发是：要把 TILE、unroll、片上 buffer 和外部带宽放在一起建模。论文里的 roofline 思想可以简化成：

```text
attainable performance = min(compute roof, CTC x memory bandwidth)
```

我现在还没有真实接 AXI/DDR，所以外部 DDR roofline 只能先估算；但 PL 内部的 roofline 可以用 HLS report 和 C/RTL cosim 分析。也就是分成两层：

```text
外部 roofline:
  DDR/AXI -> A_buf/B_buf/C_buf 是否喂得上

内部 roofline:
  A_buf/B_buf/C_buf -> localA/localB/localC -> gemm_core_mac 是否喂得上
```

当前阶段最该做的是内部 roofline，因为 O1 已经说明外层 A/B block 合并加载不是主要瓶颈，而 O2 说明 local feeding path 确实影响性能。

## 这一版新增了什么

新增脚本：

```text
python/analysis/roofline_model.py
```

运行方式：

```bash
python3 python/analysis/roofline_model.py
```

生成：

```text
reports/internal_roofline_points.csv
reports/internal_roofline_summary.md
```

这个脚本不依赖 `numpy`，只用 Python 标准库。后来我又把实验点从 Python 源码里拆出来，放到：

```text
python/analysis/roofline_experiments.csv
```

这样后面做 `TILE/BLOCK/ROW_UNROLL` sweep 时，只需要往 CSV 里加实验点，不用反复改 Python 源码。

脚本现在会统一计算：

```text
total MAC
total ops
external A/B/C traffic estimate
external CTC
compute roof, memory roof, attainable roof
actual MAC/cycle
actual ops/cycle
compute_peak_util
attainable_roof_util
roofline lower-bound cycles
latency / roof lower-bound
resource efficiency
modeled local feeding active ratio
local_model_gap
total_model_gap
```

## 公式怎么定

对 GEMM：

```text
MAC = N x K x M
ops = 2 x N x K x M
compute_roof_mac_per_cycle = TILE x TILE
compute_roof_ops_per_cycle = 2 x compute_roof_mac_per_cycle
```

外部 traffic 先按当前 scheduler 的循环顺序估算：

```text
A traffic ~= ceil(M / BLOCK_M) x N x K x 1 byte
B traffic ~= ceil(N / BLOCK_N) x K x M x 1 byte
C traffic ~= N x M x 4 bytes
```

这不是最理想 traffic，而是考虑了当前 block 调度里 A/B 会重复加载。后续如果做 `reuse_A/reuse_B`，这个公式也能直接看出 traffic 是否下降。

内部 local feeding 先用一个很粗的模型：

```text
T_localA_load  = TILE^2 / (TILE x row_unroll) = TILE / row_unroll
T_localB_load  = TILE^2 / (TILE x row_unroll) = TILE / row_unroll
T_localC_load  = TILE^2 / (TILE x row_unroll) = TILE / row_unroll
T_localC_store = TILE^2 / (TILE x row_unroll) = TILE / row_unroll
T_compute      = TILE
```

这不是为了精确替代 HLS schedule，而是为了解释为什么 O2 会提升：row_unroll=2 会把 local load/store 的模型周期从 14 降到 7，MAC 阵列空等的比例下降。

另外我把外部 roofline 的单位也理清楚了。现在脚本同时保留：

```text
mem_roof_ops_per_cycle
mem_roof_mac_per_cycle
compute_roof_ops_per_cycle
compute_roof_mac_per_cycle
actual_ops_per_cycle
actual_mac_per_cycle
```

这样后面汇报时不会把 ops/cycle 和 MAC/cycle 混在一起。

对 roofline 下界，脚本新增：

```text
external_mem_cycles_min = external_bytes / ddr_bytes_per_cycle
compute_cycles_min      = MAC / compute_roof_mac_per_cycle
roof_cycles_min         = max(external_mem_cycles_min, compute_cycles_min)
latency_over_roof_lower_bound = actual_latency / roof_cycles_min
```

当前假设 `DDR=800MB/s`、`100MHz`，也就是 `8 bytes/cycle`。因此 O0-O5 的外部 memory roof 是：

```text
external_bytes = 131072 bytes
external_mem_cycles_min = 131072 / 8 = 16384 cycles
compute_cycles_min = 2097152 / 196 = 10699.8 cycles
roof_cycles_min = 16384 cycles
```

这说明外部 DDR 上限并不是当前最先卡住的地方。

## 脚本输出结果

这次脚本输出：

```text
O0: bound=internal/scheduler-bound, actual=5.495 MAC/cycle, attainable=128.000, latency/roof=23.29x
O1: bound=internal/scheduler-bound, actual=5.495 MAC/cycle, attainable=128.000, latency/roof=23.29x
O2: bound=internal/scheduler-bound, actual=6.613 MAC/cycle, attainable=128.000, latency/roof=19.36x
O4: bound=internal/scheduler-bound, actual=2.906 MAC/cycle, attainable=128.000, latency/roof=44.04x
O5: bound=internal/scheduler-bound, actual=2.906 MAC/cycle, attainable=128.000, latency/roof=44.04x
```

表格在：

```text
reports/internal_roofline_summary.md
```

我的理解是：

```text
O1 = 外层 A/B 合并加载，没有改善 latency，说明这不是主瓶颈。
O2 = row banking/unroll=2，latency 下降，说明内部 local feeding 是瓶颈之一。
O4/O5 = helper 合并 local A/B，功能正确但性能更差，说明 HLS 生成了更复杂的 mux/端口调度。
```

最扎心的一点是：虽然 `TILE=14` 理论 compute roof 是 196 MAC/cycle，外部 DDR roof 估算也有 128 MAC/cycle，但 O2 实际只有约 `6.61 MAC/cycle`。换算下来：

```text
O2 attainable roof utilization = 5.17%
O2 compute peak utilization    = 3.37%
O2 latency / roof lower-bound  = 19.36x
```

这说明 O0/O1/O2/O4/O5 都应该被归类为：

```text
internal/scheduler-bound
```

也就是说，不是 DSP 没实例化，也不是现在 DDR 已经先卡死，而是完整 scheduler 里还有大量 load、store、边界判断、地址选择、BRAM 端口冲突、pipeline fill/drain、函数边界和 partial-sum 管理没有被当前粗模型完全解释。

我也新增了两个 gap 指标：

```text
local_model_gap = actual_latency / internal_model_cycles
total_model_gap = actual_latency / total_model_cycles
```

它们的意义是提醒我：局部 tile 模型只能解释一部分现象。如果 gap 很大，说明 HLS 真实调度里还有很多控制/地址/端口开销要继续拆。

当前更详细的结果：

| Case | attainable MAC/cycle | actual MAC/cycle | compute util | attainable util | latency/roof | local model gap | total model gap |
| --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 128.000 | 5.495 | 2.80% | 4.29% | 23.29x | 8.02x | 5.96x |
| O1 | 128.000 | 5.495 | 2.80% | 4.29% | 23.29x | 8.02x | 5.96x |
| O2 | 128.000 | 6.613 | 3.37% | 5.17% | 19.36x | 10.30x | 6.72x |
| O4 | 128.000 | 2.906 | 1.48% | 2.27% | 44.04x | 6.14x | 5.39x |
| O5 | 128.000 | 2.906 | 1.48% | 2.27% | 44.04x | 6.14x | 5.39x |

资源效率也更清楚了：

| Case | GOPS@100MHz | GOPS/DSP | GOPS/BRAM18K | GOPS/kLUT |
| --- | --- | --- | --- | --- |
| O0 | 1.099 | 0.00561 | 0.01963 | 0.02234 |
| O1 | 1.099 | 0.00561 | 0.01963 | 0.02242 |
| O2 | 1.323 | 0.00675 | 0.01575 | 0.01958 |
| O4 | 0.581 | 0.00297 | 0.01038 | 0.00696 |
| O5 | 0.581 | 0.00297 | 0.01038 | 0.00696 |

所以 O2 的绝对性能最好，但 BRAM/LUT 效率反而下降；O4/O5 则是性能和资源效率都失败。

## 下一步迭代计划

我把后续优化分成几步，每一步都不能只写代码不验证。

### O6：full-block fast path

目标：降低边界判断、比较器和 mux。

做法：

```text
如果 current_N/K/M 都等于完整 block：
  调用 compute_block_full_no_boundary()
否则：
  调用现有 boundary path
```

full path 里尽量删除这些判断：

```text
bi < current_N
bk < current_K
bj < current_M
```

验证要求：

```text
1. 先跑 C-sim，确认和 golden 完全一致。
2. 跑 C-synth，看 LUT 是否下降。
3. 跑 128 规模 C/RTL cosim，看 latency 是否下降。
4. 记录 O6a = O1 + full path，O6b = O2 + full path。
```

### O7：TILE/BLOCK/ROW_UNROLL 设计空间枚举

目标：不要迷信 `TILE=14`，找更稳的资源-性能点。

候选：

```text
TILE in {8, 10, 12, 14}
BLOCK_N/TILE in {4, 6, 8}
BLOCK_K/TILE in {4, 6, 8}
BLOCK_M/TILE in {4, 6, 8}
ROW_UNROLL in {1, 2}
```

不一定全跑完。先用 C-synth 筛掉 LUT/BRAM 明显爆的点，再对少数候选跑 C/RTL cosim。

### O8：reuse_A / reuse_B / accumulate_C 指令语义

目标：把 DianNao 的 NBin/SB/NBout 思想放进任务二。

当前 GEMM 指令是 fused GEMM：

```text
load A
load B
compute
store C
```

后续可以增加 flag：

```text
reuse_A
reuse_B
accumulate_C
store_C
```

这样 CNN weights、Transformer FFN weights、Attention K/V block 的复用可以用指令表达，而不是每次都重复加载。

### O9：外部 DDR roofline 估算

目标：上板前先估算 DDR 会不会成为瓶颈。

先不需要真实 AXI，只需要在模型里写：

```text
assumed DDR bandwidth = 800 MB/s
external memory roof = external CTC x bandwidth
```

如果 external memory roof 明显高于 compute roof，就说明当前更该优化 PL 内部；如果低于 compute roof，后续上板就要重点看 AXI burst、DMA、double buffer 和 cache flush/invalidate。

## 这一版没有做什么

我这一版没有继续改 `gemm_scheduler.cpp` 的数据通路。原因是 O3/O4/O5 已经说明盲目合并循环可能越改越差，所以这次先把分析方法固化成脚本和文档。

这次模型还不能代表真实板上 DDR/AXI，因为现在只是用 `ap_memory` 和假设带宽估计。它现在主要服务于两个目的：

```text
1. 判断当前是否 external-memory-bound。
2. 找出 actual latency 和内部 scheduler 粗模型之间还有多大 gap。
```

后续每做一个 O6/O7/O8，都应该先在 `roofline_experiments.csv` 里补一个设计点，再跑 HLS 验证。

## 我现在可以怎么跟老师说

可以这样表述：

```text
我结合 DianNao 和 FPGA'15 CNN accelerator 的思路，把当前 GEMM 优化拆成外部 roofline 和内部 roofline 两层。当前假设 DDR 带宽下，外部 roofline 给出的 attainable roof 约为 128 MAC/cycle，而目前最好的 O2 只有 6.613 MAC/cycle，只达到 attainable roof 的 5.17%，因此 O0-O5 都更像 internal/scheduler-bound。下一步我会重点分析 PL 内部的 A_buf/B_buf/C_buf 到 local tile 的喂数、BRAM banking、边界判断和地址 mux，并优先尝试 full-block fast path，再做 TILE/BLOCK/ROW_UNROLL 的小型设计空间枚举。
```
