# Phase 1 / 迭代日志 009：GEMM_TILE sweep 和性能 gap 分析

## 我这一版想解决什么

老师提醒我：不能只说 GEMM、Conv、Attention 跑通了，还要把 GEMM 并行规模做大一些，尽量把 DSP 用起来，然后比较理论性能和实际性能有没有差距，并解释原因。

所以这一版我做了一个固定规模的 GEMM benchmark sweep。我的理解是：

```text
固定同一个 GEMM 任务
  -> 只改变 GEMM_TILE 和对应的 GEMM_BLOCK_M
  -> 每一档都跑 C-sim / C-synth / C/RTL cosim
  -> 记录资源和 RTL latency
  -> 对比理论最好延时、实际延时、理论吞吐和实际吞吐
  -> 分析为什么 TILE 变大后没有完全按理论提升
```

## 一直不变的是什么

这次实验里一直不变的是 GEMM 的数学任务：

```text
C[16,96] = A[16,96] x B[96,96]
```

也就是：

```text
N = 16
K = 96
M = 96
总 MAC 数 = N * K * M = 147456
```

这里 `N/K/M` 不变，意味着每次要完成的总乘加数不变。这样比较才公平：同一个任务，换不同的 `GEMM_TILE`，看硬件资源和延时怎么变化。

其他保持不变的还有：

```text
Target FPGA = xc7z020clg400-2
Target clock = 10 ns
数据类型 = INT8 x INT8 -> INT32 accumulate
顶层接口 = ap_memory + ap_ctrl_hs
测试输入矩阵生成方式不变
golden reference 不变
```

`GEMM_BLOCK_M` 的含义是：`B_bram` 一次缓存多少个输出列方向的 B 矩阵数据，也就是按 M 维分块。它会影响：

```text
B_bram 的宽度
一次处理多少列输出
block_m_loop 要循环多少次
```

这次 sweep 里我设置为：

```text
TILE=4,  BLOCK_M=8
TILE=8,  BLOCK_M=8
TILE=12, BLOCK_M=12
TILE=14, BLOCK_M=14
```

也就是说，我并没有把 `BLOCK_M` 一口气扩大到完整的 `M=96`，所以 B 还是要分块搬运。综合报告里 BRAM 也一直是 2 个，这说明这次主要放大的是局部 MAC 阵列和 local buffer，而不是同步把片上存储带宽/容量一起放大。

## 我改了哪些地方

为了不用手动改 4 次头文件，我把 `gemm_types.h` 里的 `GEMM_TILE` 和 `GEMM_BLOCK_M` 改成可以由 Tcl 编译宏覆盖：

```cpp
#ifndef GZY_GEMM_TILE
#define GZY_GEMM_TILE 4
#endif

#ifndef GZY_GEMM_BLOCK_M
#define GZY_GEMM_BLOCK_M 8
#endif

static const int GEMM_TILE = GZY_GEMM_TILE;
static const int GEMM_BLOCK_M = GZY_GEMM_BLOCK_M;
```

默认还是 `TILE=4, BLOCK_M=8`，所以原来的 GEMM、Conv、QKV、Attention 脚本不会受影响。只有 benchmark sweep 脚本会通过 `-D` 宏临时生成不同规模的硬件。

新增文件：

```text
hls/src/gemm_bench_top.h
hls/src/gemm_bench_top.cpp
hls/tb/tb_gemm_bench.cpp
hls/scripts/run_hls_gemm_benchmark_sweep.tcl
```

`gemm_bench_top()` 只负责固定调用：

```cpp
gemm_tiled(A, B, C, 16, 96, 96, true);
```

`tb_gemm_bench.cpp` 会生成固定 A/B，调用 HLS top，再用普通 C++ 三层循环算 golden reference，最后比较 `mismatch_count`、`max_abs_error` 和 `checksum`。

## 这次怎么跑

我用一个 Tcl 脚本生成四个独立 HLS 工程：

```text
gemm_bench_tile4
gemm_bench_tile8
gemm_bench_tile12
gemm_bench_tile14
```

每个工程使用同一份源码和同一个 testbench，只是在编译时传入不同宏：

```text
-DGZY_GEMM_TILE=4   -DGZY_GEMM_BLOCK_M=8
-DGZY_GEMM_TILE=8   -DGZY_GEMM_BLOCK_M=8
-DGZY_GEMM_TILE=12  -DGZY_GEMM_BLOCK_M=12
-DGZY_GEMM_TILE=14  -DGZY_GEMM_BLOCK_M=14
```

运行命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm_benchmark_sweep.tcl
```

每一档都执行：

```text
csim_design
csynth_design
cosim_design -rtl verilog
```

C simulation 每一档都通过：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=101159936
[TB] PASS
```

## 理论指标怎么算

我这里先只保留两个最容易解释的指标。

理论最好延时：

```text
理论最好延时 =
  ceil(N / TILE) * ceil(M / TILE) * ceil(K / TILE) * TILE
```

这个指标只看最理想的计算部分，先不把搬 A、搬 B、写 C、控制开销算进去。

理论吞吐和实际吞吐：

```text
理论吞吐 = 总 MAC 数 / 理论最好延时
实际吞吐 = 总 MAC 数 / RTL latency
```

这里吞吐统一理解为 `MAC/cycle`，我不再额外列 GMAC/s、GOPS 和效率，避免表太乱。

## 实验结果

固定：

```text
N = 16
K = 96
M = 96
总 MAC 数 = 147456
```

| TILE | BLOCK_M | DSP | BRAM_18K | FF | LUT | 理论最好延时 | RTL latency | 理论吞吐 | 实际吞吐 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 4 | 8 | 16 | 2 | 1599 | 1552 | 9216 | 118720 | 16.000 | 1.242 |
| 8 | 8 | 64 | 2 | 5448 | 3441 | 2304 | 54784 | 64.000 | 2.692 |
| 12 | 12 | 144 | 2 | 11865 | 6824 | 1536 | 52972 | 96.000 | 2.784 |
| 14 | 14 | 197 | 2 | 16271 | 9393 | 1372 | 54492 | 107.475 | 2.706 |

## 先把 GEMM 内部数据流画清楚

这次分析之后，我感觉必须先把 GEMM 内部结构画清楚，不然只看 DSP 和 latency 很容易误判。

```text
外部 A/B/C 数组 ap_memory
        |
        | update_A=true 时，把 A 搬进 A_bram
        v
+-------------------+          +----------------------+
| A_bram[N][K]      |          | B_bram[K][BLOCK_M]  |
| 片上 BRAM          |          | 片上 BRAM             |
+-------------------+          +----------------------+
        ^                              ^
        |                              |
        | load_a_bram                  | 每个 j_block 都 load_b_bram
        | N*K cycles                   | K*BLOCK_M cycles
        |                              |
        +------------------------------+
                       |
                       v
          block_m_loop: 按 M 方向分块
                       |
                       v
        tile_i_loop / tile_j_loop / tile_k_loop
                       |
                       v
+-------------------+   +-------------------+   +-------------------+
| localA[T][T]      |   | localB[T][T]      |   | localC[T][T]      |
| complete partition|   | complete partition|   | complete partition|
| 寄存器阵列         |   | 寄存器阵列         |   | 寄存器阵列         |
+-------------------+   +-------------------+   +-------------------+
        |                       |                       ^
        | load_local_a          | load_local_b          |
        | 大约 T*T cycles       | 大约 T*T cycles       |
        +-----------+-----------+                       |
                    v                                   |
              dot_k 计算阵列                            |
                    |                                   |
                    | 每个 k0 大约 T cycles              |
                    | 每周期并行 T*T MAC                 |
                    v                                   |
              localC += localA * localB ----------------+
                       |
                       | write_c，大约 T*T cycles
                       v
                 外部 C 数组
```

这里最容易忽略的是：`localA/localB/localC` 做了 `ARRAY_PARTITION complete`，所以它们在计算时可以被并行访问；但把 `A_bram/B_bram` 的数据搬到 `localA/localB` 的时候，代码还是两层循环加 `PIPELINE II=1`，并不是一拍把整个 tile 全部读出来。

所以一次 `k0` 的时间线大致是：

```text
load_local_a  ->  load_local_b  ->  dot_k compute
   T*T cycles      T*T cycles          T cycles
```

换成具体数字：

```text
TILE=4:   16 + 16 + 4   = 36 cycles
TILE=8:   64 + 64 + 8   = 136 cycles
TILE=12: 144 + 144 + 12 = 300 cycles
TILE=14: 196 + 196 + 14 = 406 cycles
```

这说明一个很关键的问题：`TILE` 变大后，计算阵列确实变强了，但 local buffer 的加载成本也按 `T*T` 增长。如果 load 和 compute 没有重叠，大 TILE 很容易变成“MAC 阵列很大，但一直等数据”。

从代码里也能看出来这三个阶段是串行的：

```cpp
load_local_a:
  for ii
    for kk
      #pragma HLS PIPELINE II=1
      localA[ii][kk] = A_bram[...]

load_local_b:
  for kk
    for jj
      #pragma HLS PIPELINE II=1
      localB[kk][jj] = B_bram[...]

dot_k:
  for kk
    #pragma HLS PIPELINE II=1
    for ii
      #pragma HLS UNROLL
      for jj
        #pragma HLS UNROLL
        localC[ii][jj] += ...
```

当前没有 `DATAFLOW`，也没有双缓冲，所以不是“一边算当前 tile，一边加载下一个 tile”。

## 每一档怎么理解

### TILE=4 到 TILE=8

这一档是提升最明显的。

资源变化：

```text
DSP: 16 -> 64
FF: 1599 -> 5448
LUT: 1552 -> 3441
RTL latency: 118720 -> 54784
实际吞吐: 1.242 -> 2.692
```

代码里的原因是 `dot_k` 里 `ii` 和 `jj` 两层循环被 `UNROLL` 展开，`localA/localB/localC` 又做了 `ARRAY_PARTITION complete`。所以 `TILE=8` 时，局部 MAC 阵列从 `4x4=16` 路变成 `8x8=64` 路，DSP、FF、LUT 都同步增长。

延时明显下降，说明从 4 到 8 确实增加了有效并行度。我的理解是，`TILE=4` 时计算阵列偏小，很多时间还是被计算本身占住；扩大到 `TILE=8` 后，tile 数减少，计算阵列也从 16 路扩到 64 路，所以计算瓶颈被明显缓解。

但它没有按理论从 16 路变 64 路那样得到 4 倍提升。原因是 RTL latency 不只包含 `dot_k` 计算，还包含：

```text
load_a_bram
load_b_bram
load_local_a
load_local_b
write_c
block_m_loop / tile_i_loop / tile_j_loop 控制开销
```

这些部分没有因为 `TILE` 变大而同步按比例优化。

换句话说，`TILE=4 -> 8` 会优化，是因为原来确实有一部分瓶颈在 MAC 阵列太小。

### TILE=8 到 TILE=12

这一档资源继续明显增长，但延时几乎没有继续大幅下降。

资源变化：

```text
DSP: 64 -> 144
FF: 5448 -> 11865
LUT: 3441 -> 6824
RTL latency: 54784 -> 52972
实际吞吐: 2.692 -> 2.784
```

代码里的 MAC 阵列确实从 `8x8=64` 路变成了 `12x12=144` 路，所以 DSP 增长是合理的。

但是这里开始出现很明显的问题：`N=16` 太小了，`TILE=12` 时 N 方向只分成两个 tile，第二个 tile 只有 4 行有效，很多 MAC lane 会空转。

更重要的是，`TILE=12` 的 local 加载成本也变大了：

```text
load_local_a + load_local_b = 144 + 144 = 288 cycles
dot_k compute = 12 cycles
```

也就是说，计算本身很短，但准备 localA/localB 的时间很长。`A_bram/B_bram` 仍然是通过双端口 BRAM 和 local buffer 搬运，没有同步扩大访问带宽，也没有做 DATAFLOW 或双缓冲，所以更大的 MAC 阵列喂不满。

所以这一档虽然理论最好延时从 2304 降到 1536，但实际 RTL latency 只从 54784 降到 52972，说明瓶颈已经不主要在 MAC 数量了。

### TILE=12 到 TILE=14

这一档 DSP 接近吃满，但实际延时反而略变差。

资源变化：

```text
DSP: 144 -> 197
FF: 11865 -> 16271
LUT: 6824 -> 9393
RTL latency: 52972 -> 54492
实际吞吐: 2.784 -> 2.706
```

`TILE=14` 理论上应该是 `14x14=196` 路 MAC。综合结果里 DSP 是 197，是因为除了 196 个主要 MAC，HLS 还额外生成了一个和地址/循环相关的乘加资源。

这档没有继续优化，主要原因是：

```text
N=16 对 TILE=14 很不友好
第二个 N tile 只有 2 行有效
M=96 对 TILE=14 也不是整除
K=96 对 TILE=14 也不是整除
B 仍然按 BLOCK_M=14 分块搬运，不是一次缓存完整 M=96
BRAM 数量仍然是 2，片上存储访问能力没有同步放大
local buffer 和 mux/address/control 逻辑更复杂
```

`TILE=14` 时，单次 `k0` 里：

```text
load_local_a + load_local_b = 196 + 196 = 392 cycles
dot_k compute = 14 cycles
```

所以更大的 MAC 阵列大部分时间并没有持续工作。`TILE=14` 的意义主要是证明 DSP 可以接近打满，而不是证明当前结构下性能最好。

## 这次我发现的问题

我一开始以为只要把 `GEMM_TILE` 变大，实际 latency 就应该明显下降。但这次结果说明不是这样。

我现在的理解是：

```text
TILE 变大
  -> MAC 阵列变大
  -> DSP/FF/LUT 变大

但如果 N/K/M、GEMM_BLOCK_M、BRAM 访问带宽、local buffer 搬运方式没有同步优化
  -> 更大的 MAC 阵列就会等数据
  -> 实际吞吐不会按理论峰值增长
```

换句话说，这次测试主要放大了计算阵列，没有同步放大片上数据供给能力。尤其 `N=16` 固定不变，对 `TILE=12/14` 不太公平，因为矩阵行数太小，不能充分填满大 tile。

我现在更准确的判断是：

```text
TILE=4 -> 8：
  主要改善了 MAC 阵列太小的问题，所以 latency 明显下降。

TILE=8 -> 12/14：
  MAC 阵列继续变大，但 load_local_a/b、B 分块搬运、边界空转和控制逻辑变成瓶颈，
  所以 DSP 继续增长，但实际吞吐基本卡住。
```

这里“哪里吃满了”不能简单说 BRAM 容量吃满，因为 BRAM 数量还只有 2；更准确地说，是当前数据供给结构吃紧：BRAM 端口和 local buffer 加载方式没有办法按 `T*T` 的并行 MAC 规模同步喂数据。

所以如果后续继续做性能优化，我觉得应该分两条线：

```text
1. 先做更公平的 benchmark：
   选择更大的 N/M，或者选择能整除 TILE 的 N/K/M，
   让 TILE=12/14 不至于大量边界空转。

2. 再测试 GEMM_BLOCK_M：
   例如 BLOCK_M=16 或 32，观察减少 B 分块次数后 latency 会不会下降。
   我暂时不想直接把 BLOCK_M 拉到 96，因为这会占更多 BRAM 和控制逻辑，
   而且最后 Conv/Attention/accelerator_top 还要留资源。

3. 真正优化 GEMM 结构：
   load_local_a / load_local_b 和 dot_k 做 DATAFLOW 或双缓冲，
   减少 MAC 阵列等待数据的时间，
   同时重新考虑 GEMM_BLOCK_M 和 B_bram 的组织方式。
```

这一版的结论不是“越大越好”，而是：`TILE=8` 开始已经能看到并行带来的收益；`TILE=12/14` 虽然 DSP 用得更多，但当前数据搬运和固定 N/K/M 限制了实际性能，需要进一步优化访存和分块结构。
