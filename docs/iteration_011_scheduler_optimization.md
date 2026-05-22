# Iteration 011：64-bit 指令、TILE=14 和 scheduler 调度优化

## 我这一版想解决什么

这次我开始把前一版 V1/V2/V3 往老师说的方向再推进一点：不要只停留在“功能跑通”，而是开始动调度路径，观察计算阵列、片上缓存和供数之间的关系。

我这一版先做了几件事：

```text
1. 把指令字从 128 bit 改成 64 bit。
2. 把 accelerator 路线的默认测试参数改成 TILE=14、BLOCK_N/K/M=112。
3. 把 scheduler 优化做成可配置宏，方便复现实验阶段。
4. 尝试 A/B block 并行加载。
5. 尝试 local buffer 行方向 banking + row unroll=2。
```

我现在更清楚了：`gemm_core_mac()` 本身已经是比较“并行”的 2D MAC 阵列；真正拖慢整体性能的地方，很可能是 `gemm_scheduler()` 里怎么把数据搬到 `A_buf/B_buf/C_buf`，再怎么从 BRAM 喂给 `localA/localB/localC`。

## 阶段 0：指令改成 64 bit

之前的指令是 128 bit，字段比较宽。这次我改成了 64 bit：

```text
[7:0]     opcode
[19:8]    N - 1
[31:20]   K - 1
[43:32]   M - 1
[49:44]   a_base / 4096
[55:50]   b_base / 4096
[61:56]   c_base / 4096
[63:62]   reserved
```

这里我把 `N/K/M` 存成 `value - 1`，这样 12 bit 字段可以表示 1 到 4096。当前测试的 `1024` 可以正常放进去。

base address 这里我先做了一个简化：因为当前 HLS 单元验证里 `A_mem/B_mem/C_mem` 还是分开的三个数组，所以 base 主要是为了保留“以后接 DDR 指令”的形式感；现在字段按 4096 element 对齐。后面如果真的做统一 DDR 地址空间，这里可能要扩展成多条配置指令，或者把 base 放进寄存器而不是一条 64-bit GEMM 指令里。

我这一版学到的是：64-bit 指令不一定能把所有信息都塞得很舒服，所以指令集不是越“像 CPU”越好，而是要结合具体 accelerator 的寄存器配置和内存模型来设计。

## 阶段 1：TILE=14，BLOCK=112

这一步先不改调度，只把参数从：

```text
TILE = 12
BLOCK = 96
```

换成：

```text
TILE = 14
BLOCK_N/K/M = 112
```

这样 `gemm_core_mac()` 理论上会使用约：

```text
14 x 14 = 196 MAC
```

也就是更接近把 ZYNQ-7020 的 DSP 用满。这个阶段的意义不是保证 latency 一定下降，而是观察当计算阵列变大后，scheduler 能不能喂得上。

## 阶段 2：A/B block 并行加载

原来的调度是：

```text
load_a_block()
load_b_block()
compute_block()
```

这里 `A_mem` 和 `B_mem` 是两个不同数组，`A_buf` 和 `B_buf` 也是两个不同片上 buffer，所以 A 和 B 的 block 加载本身没有数据依赖。

我新增了：

```cpp
load_ab_block()
```

让它在同一个 pipeline 循环里同时做：

```text
A_mem -> A_buf
B_mem -> B_buf
```

我理解这属于“部分并行”：它还没有把 load 和 compute 重叠，只是把 A block 和 B block 的两段串行加载合并成同一段加载。理论上，如果 A/B 两边接口和 BRAM 写口都能支持，它可以把 block 加载时间从接近：

```text
load A + load B
```

压到接近：

```text
max(load A, load B)
```

## 阶段 3：local row banking + row unroll=2

在 `compute_block()` 里面，每个 K tile 都会做：

```text
load_local_a
load_local_b
gemm_core_mac
```

之前虽然 `localA/localB/localC` 是 complete partition，但 `A_buf/B_buf/C_buf` 仍然主要按列方向 partition。这样 local load 还是需要按行一行一行搬：

```text
TILE=14 时，大概每次 localA 要 14 拍，localB 要 14 拍
```

这次我加了一个比较保守的 banking：

```cpp
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=2 dim=1
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=2 dim=1
#pragma HLS ARRAY_PARTITION variable=C_buf cyclic factor=2 dim=1
```

同时把 local load/store 的行方向改成 `row_unroll=2`。也就是说，我不是一下子全展开 14 行，而是先一次搬 2 行。这样资源压力比“全并行 local load”低很多，但能观察 banking 是否对 local load 有帮助。

我没有直接做 `row_unroll=14`，因为这会要求 BRAM bank 和读端口一下子爆炸，可能把 BRAM、mux 和布线压力拉得很高。对 ZYNQ-7020 来说，先做 `2` 是更稳一点的实验。

## 新增的可配置宏

为了让每一版可以复现，我没有把优化写死，而是加了宏：

```text
GZY_ACCEL_LOAD_AB_PARALLEL
  0: load_a_block + load_b_block 串行
  1: load_ab_block 合并加载

GZY_ACCEL_LOCAL_ROW_UNROLL
  1: 原来的 local row 加载方式
  2: 行方向多开一组 bank，并一次搬 2 行
```

这样同一份源码可以跑：

```text
O0: TILE=14, BLOCK=112, serial load
O1: TILE=14, BLOCK=112, A/B block 并行加载
O2: TILE=14, BLOCK=112, A/B block 并行加载 + row banking=2
```

## 实际 HLS 验证结果

这一轮已经可以直接调用 Windows 版 Vitis HLS 跑 Tcl。命令形式是：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log11_o5_scheduler.tcl
```

我这次没有再把每一个小想法都塞进 V1/V2/V3 全套顶层里，而是先固定测试：

```text
N = 128, K = 128, M = 128
total MAC = 2097152
TILE = 14
BLOCK_N/K/M = 112
```

这样规模不算小，RTL cosim 还能跑完，也方便观察 scheduler 本身的问题。

| Case | 主要改动 | C-sim | C-synth | C/RTL cosim | BRAM_18K | DSP | FF | LUT | RTL latency | MAC/cycle |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 串行加载基线 | PASS | PASS | PASS | 56 | 196 | 33470 | 49206 | 381634 | 5.50 |
| O1 | A/B block 合并加载 | PASS | PASS | PASS | 56 | 196 | 33282 | 49023 | 381634 | 5.50 |
| O2 | O1 + row banking=2 | PASS | PASS | PASS | 84 | 196 | 34317 | 67546 | 317122 | 6.61 |
| O3 | 尝试把 local A/B load 合成一个循环 | PASS | PASS | 未跑长 cosim | 56 | 196 | 43062 | 27623 | 2979010 synth | 0.70 |
| O4 | local A/B helper 函数 | PASS | PASS | PASS | 56 | 196 | 40296 | 83514 | 721602 | 2.91 |
| O5 | helper 形参加 partition | PASS | PASS | PASS | 56 | 196 | 40296 | 83514 | 721602 | 2.91 |

O5 的 C-sim/cosim 输出：

```text
[V1] N=128 K=128 M=128 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=2097152
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=35200361
[V1] PASS
```

## 这轮实验我看到的问题

O1 没有比 O0 快，说明 `A_mem -> A_buf` 和 `B_mem -> B_buf` 这段合并加载不是当前最主要瓶颈。虽然代码结构上更并行了，但整体 latency 没降。

O2 latency 从 `381634` 降到 `317122`，说明 local buffer 喂给 `localA/localB` 的阶段确实会卡住。row banking=2 能把部分 local load 从 14 拍压到 7 拍左右，所以实际有收益。但代价也很明显：LUT 到了 `67546`，超过 ZYNQ-7020 的可用 LUT 数量，这个版本只能说明方向有用，不能直接作为最终版本。

O3/O4/O5 是我想进一步把 `load_local_a` 和 `load_local_b` 合并的尝试。结果并不好：

```text
load_local_ab_tile:
  Target II = 1
  Final II  = 7
```

HLS 报告里明确提示 `A_buf` memory ports 不够。即使 O5 在 helper 形参上补了 `ARRAY_PARTITION`，最终还是出现大量 `mux` 和 `urem`，LUT 到 `83514`，latency 也比 O0 更差。这个结果说明：把两个 load 硬合到一个 helper 里，并不会自动得到并行，反而可能让 HLS 的地址选择和端口调度更复杂。

我这次学到的是：`UNROLL/PIPELINE/ARRAY_PARTITION` 不是孤立生效的。只有 bank 数、端口数、访问模式和循环结构都对得上，展开才会真的变快；否则就是用更多 LUT 生成更复杂的 mux 和控制逻辑。

## 这一版我对“是否每一步都跑 V1/V2/V3”的理解

我现在觉得没必要每一个 scheduler 小优化都完整跑 V1/V2/V3 三套大规模验证。更合理的是：

```text
改 scheduler 本体：
  先跑 V1，因为 V1 直接测 gemm_scheduler。

改 instruction/decode：
  跑 V2。

改 accelerator_top 接口：
  跑 V3。

最终阶段：
  V1/V2/V3 都跑一遍小规模 RTL cosim；
  V1/V3 再跑一遍 1024 C-sim/C-synth。
```

这也是这次 Tcl 的组织方式。这样既能覆盖功能，又不会每次都做非常耗时的重复验证。

## 后续想法

这轮之后我觉得下一步不应该继续沿着 O4/O5 的 helper 合并方向走。更值得做的是：

```text
1. 保留 O0/O1 这种资源相对可控的版本作为基线。
2. 继续研究 O2 的 bank/unroll 思路，但要想办法降低 LUT。
3. 把完整 block 和边界 block 分开，减少每个循环里的 if/mux。
4. 用自增地址减少非 2 的幂 block 下的 urem/div 地址逻辑。
5. 后面再考虑 double buffer 或 DATAFLOW，让 load/compute/store 真正重叠。
```

我现在也更谨慎了：全并行并不是“把 pragma 全部加满”。如果 bank 数量不够，UNROLL 只会生成巨大的 mux 和冲突；如果 bank 数量太多，又可能把 BRAM 和布线压力拉爆。所以后续每一步都应该看资源和 latency，而不是只看 DSP 数量。
