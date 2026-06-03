# Phase 3 Iteration 025：从资源模型回到 HLS buffer banking 优化

## 我这一版想解决什么

前面已经把第 3 层板级链路跑通：

```text
PS -> AXI-Lite -> PL -> AXI master/HP -> DDR -> GEMM -> DDR -> PS golden check
```

`4x4`、`16x16`、`112x112` 都已经能在板上通过。接下来我原本想继续把矩阵放大到 `1008/1024`，但是 HLS 里先暴露出一个更现实的问题：

```text
TILE=14 / BLOCK=112 / MAX=1024 的 generic boundary 版本，LUT 已经接近或超过 ZYNQ-7020 上限。
```

所以这一版先不继续 Vivado/Vitis 上板，而是回到 HLS report、latency 模型和源码结构里，把“LUT 到底花在哪里”看清楚，再决定下一步优化。

## 我不是直接拍脑袋换方向

这次判断主要来自三类信息对照：

```text
1. HLS C-synth report 的资源层级；
2. 之前写的 block/tile latency 模型；
3. gemm_scheduler.cpp 里 load/compute/store 的实际访问方式。
```

之前的粗模型把总延迟拆成：

```text
T_total =
  T_load_AB_block
+ T_compute_block_internal
+ T_store_C_block
+ T_control
```

对照 HLS report 后可以看到：

| 模块 | 当前作用 | HLS latency 观察 | 资源观察 |
| --- | --- | --- | --- |
| `load_ab_block` | DDR A/B -> A_buf/B_buf | 原并行写法 II=2 | 不是 LUT 主因 |
| `compute_block` | A/B/C buffer -> local tile -> MAC -> C buffer | tile 内部调度开销最大 | LUT 主因 |
| `store_c_block` | C_buf -> DDR C | II=1 | 资源很小 |
| instruction/control | 指令译码和调度 | 不是主瓶颈 | 资源很小 |

这说明当前真正危险的不是 AXI-Lite、AXI master，也不是单纯的 DDR load/store，而是 `compute_block()` 内部的片上 buffer banking、local tile 喂数、边界补零和 mux。

## 当前 1024 版资源情况

当前 1024 generic 版的顶层 HLS 估计大致是：

```text
BRAM18K = 60 / 280
DSP     = 199 / 220
FF      = 36892 / 106400
LUT     = 53581 / 53200
```

LUT 已经非常紧，甚至超过报告里的可用 LUT 数。

更关键的是层级分布：

```text
accelerator_top_axi          ~= 53581 LUT
execute_instruction_stream   ~= 52077 LUT
gemm_scheduler               ~= 51632 LUT
compute_block                ~= 46608 LUT
gemm_core_mac                ~=  5210 LUT
```

这说明 `14x14` MAC 阵列本身不是 LUT 最大来源。真正的大头在 `compute_block` 外围。

## 我看到的主要 LUT 来源

`compute_block` 里最值得关注的是几类 mux：

```text
mux_164_8_1_1      约 392 个，每个约 65 LUT，总计约 25480 LUT
mux_144_32_1_1     约 14 个，每个约 57 LUT
HLS Multiplexer    约 13166 LUT
```

我现在的理解是：

```text
localA[14][14] 有 196 个 int8 元素
localB[14][14] 有 196 个 int8 元素
196 + 196 = 392
```

这些大 mux 很可能来自：

```text
1. 从 cyclic-partition 的 A_buf/B_buf 里选择正确 bank；
2. 运行时判断 current_N/current_K/current_M；
3. 越界时选择真实数据还是 0；
4. localC 是否从 C_buf 读旧值，还是 reset 成 0；
5. C_buf 写回时的条件选择和状态控制。
```

也就是说，现在不是 DSP 没展开，而是为了把数据喂给 `196` 个 MAC，HLS 生成了很多 bank select / boundary select / data-or-zero 的逻辑。

## 串行 load 实验带来的结论

我还试了一版：

```text
GZY_ACCEL_LOAD_AB_PARALLEL = 0
```

也就是不用 `load_ab_block()`，改成顺序调用：

```text
load_a_block()
load_b_block()
compute_block()
```

结果是：

| 配置 | load II | load latency | 顶层 LUT |
| --- | --- | ---: | ---: |
| 原 `load_ab_block` | II=2 | 25,101 | 53,581 |
| 串行 `load_a + load_b` | II=1 + II=1 | 12,557 + 12,557 | 53,932 |

这说明：

```text
1. 串行后每个 load loop 的 II 确实变成了 1；
2. 但 A 和 B 顺序加载，总 load latency 基本没有下降；
3. 顶层 LUT 反而略微增加；
4. compute_block 仍然是 46608 LUT，没有被这个改动影响。
```

所以 `load_ab_block` 不是当前资源超预算的主要原因。后面不应该继续围绕外层 load 做小改小补。

## 后续可继续尝试的方向：显式 banked buffer

当前写法是：

```cpp
static gemm_data_t A_buf[112][112];
static gemm_data_t B_buf[112][112];
static gemm_acc_t  C_buf[112][112];

#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=14 dim=2
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=14 dim=2
#pragma HLS ARRAY_PARTITION variable=C_buf cyclic factor=14 dim=2
```

compute 阶段访问：

```cpp
A_buf[ti + ii][tk + kk]
B_buf[tk + kk][tj + jj]
C_buf[ti + ii][tj + jj]
```

理论上，因为 `tk`、`tj` 每次都按 `14` 增加：

```text
(tk + kk) % 14 = kk
(tj + jj) % 14 = jj
```

也就是说，`kk/jj` 对应的 bank 号本来应该是固定的。但是 HLS 面对运行时 index、边界判断和 if/else 时，不一定能完全证明这一点，于是可能生成大量动态 bank select mux。

在先尝试把 compute 边界判断移出去之后，如果 LUT 仍然主要卡在 bank select mux，那么后续可以继续试显式把 bank 维度写进数组：

```cpp
A_bank[14][112][8]
B_bank[14][112][8]
C_bank[14][112][8]
```

含义是：

```text
A_bank[k_bank][i][k_group]
B_bank[j_bank][k][j_group]
C_bank[j_bank][i][j_group]
```

这样 compute 阶段可以写成更明确的 lane 访问：

```cpp
localA[ii][kk] = A_bank[kk][ti + ii][tk / 14];
localB[kk][jj] = B_bank[jj][tk + kk][tj / 14];
localC[ii][jj] = C_bank[jj][ti + ii][tj / 14];
```

这里 `kk/jj` 是 unroll 后的 lane。这样写的目的不是改变矩阵布局，而是减少 HLS 自己推断 cyclic partition bank 时生成的大 mux。

## 为什么保留这个方向，而不是直接降 TILE

直接把 `TILE=14` 降到 `TILE=12` 肯定能降资源：

```text
14 x 14 = 196 MAC
12 x 12 = 144 MAC
```

但这也会直接降低计算阵列规模，latency 很可能明显增加。我的目标不是简单把资源降下来，而是在 ZYNQ-7020 资源尽量用满的前提下，把 latency 尽量压低。

所以比起直接降 TILE，更合理的后续路线是：

```text
先通过 boundary hoist 去掉 compute feeding 里的重复边界判断；
如果资源仍然超，再尝试显式 bank，继续保住 TILE=14；
如果显式 bank 仍然无法过资源，再降到 TILE=12/BLOCK=96。
```

`TILE=13` 暂时不优先考虑，因为它和 `BLOCK=112` 不整除，block 内 tile 调度会不规整。真要降 tile，`TILE=12/BLOCK=96` 更干净。

## 后续简单路线

在 boundary hoist 实验之后，如果还需要继续压 LUT，这一版可以按这个顺序做：

```text
1. 新建一版 HLS 脚本，不覆盖当前 1024 baseline。
2. 只改片上 A/B/C buffer 的显式 bank 布局，先保持 TILE=14/BLOCK=112/MAX=1024。
3. 跑 C-sim，确认功能和原来一致。
4. 跑 C-synth，重点看：
   - top LUT 是否低于 53200；
   - compute_block LUT 是否下降；
   - mux_164_8_1_1 数量是否下降；
   - DSP 是否仍保持 196 左右；
   - BRAM 是否仍在可接受范围内。
5. 如果资源通过，再考虑导出 IP 和上板验证 1008/1024。
6. 如果资源仍然超，再做 TILE=12/BLOCK=96 的 fallback 版本。
```

这轮的核心目标不是马上追求更快，而是先解决：

```text
TILE=14 是否能在 generic boundary + MAX=1024 下真正放进 ZYNQ-7020。
```

如果显式 bank 能把 LUT 拉回预算内，它会比直接降 TILE 更有价值，因为它保留了 `14x14` 的 MAC 阵列规模。

## 实际继续做的修改：在 boundary hoist 后加显式 bank 实验

后面我没有直接覆盖 `boundary_hoist` 这一版，而是单独加了一版实验：

```text
hls/scripts/run_hls_accel_axi_1024_explicit_banks.tcl
```

它的关键宏是：

```text
GZY_ACCEL_COMPUTE_PADDED_INPUTS = 1
GZY_ACCEL_EXPLICIT_BANKS       = 1
GZY_ACCEL_FULL_ONLY            = 0
GZY_ACCEL_FULL_BLOCK_FAST      = 0
GZY_ACCEL_LOCAL_DOUBLE_BUFFER  = 0
GZY_ACCEL_LOCAL_AB_PARALLEL    = 0
GZY_ACCEL_LOCAL_AB_DIRECT      = 0
GZY_ACCEL_LOAD_AB_PARALLEL     = 1
```

我这样限制组合，是为了让这次实验只回答一个问题：

```text
在已经做完 boundary hoist 的基础上，把 A/B/C 片上 buffer 从 cyclic partition
改成显式 bank，到底还能不能继续减少 bank/address 相关 mux。
```

也就是说，这一版不是 local double buffer、不是 full-only、不是 dataflow overlap，而是一个很窄的资源模型验证。

源码里对应加了三个新的 banked 函数：

```text
load_ab_block_banked()
compute_block_banked()
store_c_block_banked()
```

原来的二维 buffer 是：

```cpp
A_buf[112][112]
B_buf[112][112]
C_buf[112][112]
```

新实验里的片上 buffer 是：

```cpp
A_bank[14][112][8]
B_bank[14][112][8]
C_bank[14][112][8]
```

这里 `8 = 112 / 14`，所以第三维是 group 号。具体含义是：

```text
A_bank[k_bank][i][k_group]
B_bank[j_bank][k][j_group]
C_bank[j_bank][i][j_group]
```

这样 compute 阶段就可以把原来的访问：

```cpp
localA[ii][kk] = A_buf[ti + ii][tk + kk];
localB[kk][jj] = B_buf[tk + kk][tj + jj];
localC[ii][jj] = C_buf[ti + ii][tj + jj];
```

改成更直接的 lane/bank 访问：

```cpp
localA[ii][kk] = A_bank[kk][ti + ii][tk / 14];
localB[kk][jj] = B_bank[jj][tk + kk][tj / 14];
localC[ii][jj] = C_bank[jj][ti + ii][tj / 14];
```

这个修改的重点不是改 DDR 里的矩阵布局。DDR 里的 A/B/C 仍然按原来的线性矩阵顺序存放：

```text
A_mem[a_base + (n0 + i) * K + (k0 + k)]
B_mem[b_base + (k0 + k) * M + (m0 + j)]
C_mem[c_base + (n0 + i) * M + (m0 + j)]
```

真正变化的是 PL 内部的 buffer 存法。`load_ab_block_banked()` 在加载时把二维坐标拆成：

```text
k  -> kb = k % 14, kg = k / 14
j  -> jb = j % 14, jg = j / 14
```

然后写入：

```cpp
A_bank[kb][i][kg]
B_bank[jb][k][jg]
```

最后 `store_c_block_banked()` 再从：

```cpp
C_bank[jb][i][jg]
```

还原成 DDR 里的：

```cpp
C_mem[c_base + (n0 + i) * M + (m0 + j)]
```

所以 C-sim 如果通过，就说明这个显式 bank 只改变了片上布局，没有改变 GEMM 的数学结果。

## 验证命令和 C-sim 结果

这版的运行命令是：

```bat
cd C:\Transformer\gzy_gemm_accel
C:\Xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_axi_1024_explicit_banks.tcl
```

脚本里仍然让硬件容量保持 1024：

```text
GZY_ACCEL_MAX_N = 1024
GZY_ACCEL_MAX_K = 1024
GZY_ACCEL_MAX_M = 1024
```

但是 C-sim bench 仍然用 112：

```text
GZY_ACCEL_BENCH_N = 112
GZY_ACCEL_BENCH_K = 112
GZY_ACCEL_BENCH_M = 112
```

这样做是为了避免 C-sim 跑完整 `1024^3` 软件仿真太慢，同时综合出来的 m_axi depth 和内部 MAX 仍然是 1024 级别。

C-sim 的关键输出是：

```text
[AXI] N=112 K=112 M=112 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=1404928
[AXI] status=1 expected_status=1
[AXI] mismatch_count=0
[AXI] max_abs_error=0
[AXI] checksum=20974123
[AXI] PASS
```

这说明 `A_bank/B_bank/C_bank` 的地址重排没有破坏结果。

## 三版资源对比：generic -> boundary hoist -> explicit banks

我把三版放在一起看：

| 版本 | BRAM18K | DSP | FF | LUT | 是否过 53200 LUT |
| --- | ---: | ---: | ---: | ---: | --- |
| `accel_axi_o1_1024` generic boundary | 60 | 199 | 36892 | 53581 | 否，超 381 |
| `accel_axi_o1_1024_boundary_hoist` | 60 | 199 | 33302 | 23708 | 是 |
| `accel_axi_o1_1024_explicit_banks` | 60 | 200 | 33205 | 22887 | 是 |

这里最大的结论是：

```text
boundary hoist 才是主收益：
top LUT: 53581 -> 23708，减少 29873 LUT

explicit banks 是在主收益之后继续压一点：
top LUT: 23708 -> 22887，再减少 821 LUT
```

从最初 generic 到 explicit banks，总变化是：

```text
top LUT: 53581 -> 22887，减少 30694 LUT
```

也就是说，`TILE=14 / BLOCK=112 / MAX=1024` 现在已经从 LUT 超预算变成了大约 `43% LUT`，资源上明显可放进 ZYNQ-7020。

## 层级资源拆开看：具体是哪一小块变了

这次最有用的是层级对比。先看顶层到 scheduler 的链条：

| 模块 | generic LUT | boundary LUT | explicit LUT | generic -> boundary | boundary -> explicit |
| --- | ---: | ---: | ---: | ---: | ---: |
| `accelerator_top_axi` | 53581 | 23708 | 22887 | -29873 | -821 |
| `execute_instruction_stream` | 52077 | 22204 | 21383 | -29873 | -821 |
| `gemm_scheduler` | 51632 | 21759 | 20938 | -29873 | -821 |
| `compute_block / compute_block_banked` | 46608 | 16735 | 15604 | -29873 | -1131 |
| `load_ab_block / load_ab_block_banked` | 1570 | 1570 | 1674 | 0 | +104 |
| `store_c_block / store_c_block_banked` | 988 | 988 | 1194 | 0 | +206 |
| `gemm_core_mac` | 5210 | 5210 | 5210 | 0 | 0 |

这个表说明得比较清楚：

```text
1. generic -> boundary 的 29873 LUT，几乎全部来自 compute_block；
2. boundary -> explicit 的 compute_block 本身又少了 1131 LUT；
3. 但是显式 bank 让 load/store 变复杂了：
   load_ab 多 104 LUT，store_c 多 206 LUT；
4. 所以顶层净收益不是 1131，而是 1131 - 104 - 206 = 821 LUT。
```

这也符合直觉：显式 bank 让 compute 访问更直接，但 load/store 必须负责二维布局和显式 bank 布局之间的转换，所以 load/store 会付出一点地址和控制逻辑成本。

## compute_block 内部再拆开

只看 compute 模块本身：

| 资源小类 | generic compute | boundary compute | explicit compute | generic -> boundary | boundary -> explicit |
| --- | ---: | ---: | ---: | ---: | ---: |
| Expression LUT | 1254 | 648 | 167 | -606 | -481 |
| Instance LUT | 32188 | 6708 | 6058 | -25480 | -650 |
| HLS Multiplexer LUT | 13166 | 9379 | 9379 | -3787 | 0 |
| Register FF | 25716 | 22126 | 22074 | -3590 | -52 |
| Total LUT | 46608 | 16735 | 15604 | -29873 | -1131 |

这里可以细化理解：

```text
boundary hoist 主要减少了 Instance LUT 和 HLS Multiplexer。
explicit banks 没有继续减少 HLS Multiplexer，那一项仍然是 9379 LUT；
它主要继续减少 Expression LUT 和 Instance LUT。
```

其中 `generic -> boundary` 的 Instance LUT 减少非常大：

```text
32188 -> 6708，减少 25480 LUT
```

这正好对应原来那批：

```text
mux_164_8_1_1: 392 个，每个 65 LUT
392 * 65 = 25480 LUT
```

也就是说，boundary hoist 把 `localA/localB` 喂数时的运行时边界选择去掉以后，那 392 个 8-bit 大 mux 直接消失了。

而 `boundary -> explicit` 的 compute 变化是：

```text
Expression LUT: 648 -> 167，减少 481
Instance LUT:   6708 -> 6058，减少 650
Register FF:   22126 -> 22074，减少 52
```

这说明显式 bank 没有继续动到最大的 HLS Multiplexer 结构，而是减少了一部分地址计算、比较/选择表达式，以及少量实例化小逻辑。

## mux 级别的变化

我把 report 和生成 Verilog 里的 mux 也对了一遍：

| mux 类型 | generic | boundary hoist | explicit banks | 说明 |
| --- | ---: | ---: | ---: | --- |
| `mux_164_8_1_1` | 392 个 | 0 个 | 0 个 | boundary hoist 消掉的最大一批 |
| `mux_144_32_1_1` in compute | 14 个 | 14 个 | 14 个 | 三版都还在 |
| `mux_1464_32_1_1` in store | 1 个 | 1 个 | 0 个 | explicit banks 后消失 |
| `mux_144_8_1_1` in `gemm_core_mac` | 28 个 | 28 个 | 28 个 | MAC 核内部保持不变 |

这个结果比我一开始预期更具体：

```text
1. `mux_164_8_1_1` 并不是 explicit banks 消掉的，而是 boundary hoist 已经消掉了；
2. explicit banks 没有减少 compute 里的 14 个 `mux_144_32_1_1`；
3. explicit banks 消掉了 store 里的 `mux_1464_32_1_1`；
4. `gemm_core_mac` 的 28 个 `mux_144_8_1_1` 完全没有变化，说明 MAC 阵列本身没有被这次 buffer banking 改动影响。
```

我现在对 `mux_144_32_1_1` 的理解是：它更像是 `localC` 这类 32-bit 累加值在 tile/循环状态之间选择造成的 mux，而不是 A/B 的 8-bit 喂数边界 mux。所以显式 bank 后它仍然存在。

## latency 对比

顶层报告里的 1024 最坏 latency 主要受运行时 N/K/M 循环上界估算影响，所以数值非常大：

| 版本 | top worst latency cycles |
| --- | ---: |
| generic | 87755359000 |
| boundary hoist | 87755359000 |
| explicit banks | 87756826568 |

这个 top worst latency 不是这轮最适合解读的指标，因为它是 HLS 对 `N/K/M` 运行时循环的保守估计，且 explicit banks 的 load/store 索引形式变化会影响估计式。

更应该看单个 block 的三个主要阶段：

| 模块 | generic latency | boundary latency | explicit latency |
| --- | ---: | ---: | ---: |
| `load_ab_block` / `load_ab_block_banked` | 25101 | 25101 | 25102 |
| `compute_block` / `compute_block_banked` | 28865 | 28865 | 28865 |
| `store_c_block` / `store_c_block_banked` | 12562 | 12562 | 12555 |

所以这轮显式 bank 的结论是：

```text
latency 基本不变，主要收益是资源下降。
```

再细一点看 compute 内部：

```text
compute tile_i/tile_j 总 latency = 28864 cycles
tile 数量 = (112 / 14) * (112 / 14) = 8 * 8 = 64
每个 output tile iteration latency = 451 cycles
K 方向 tile 数 = 112 / 14 = 8
每个 K tile iteration latency = 52 cycles
load localA = 14 cycles, II=1
load localB = 14 cycles, II=1
gemm_core_mac = 18 cycles
store localC = 14 cycles, II=1
```

这说明显式 bank 没有改变 tile 调度结构：

```text
64 个 C tile，每个 tile 做 8 个 K tile；
每个 K tile 还是 loadA/loadB + 14x14 MAC；
MAC 阵列仍然是 196 DSP 级别的 14x14 结构。
```

## BRAM 和 DSP 的变化

BRAM 没变：

```text
三版 top BRAM18K 都是 60
```

这是因为显式 bank 没有增加总存储容量，只是把原来的：

```text
112 x 112
```

拆成：

```text
14 x 112 x 8
```

总元素个数不变。

scheduler 报告里可以看到 explicit banks 的 memory 形态：

```text
A_bank: 14 个 bank，每个 896 words x 8 bit，每个 1 BRAM18K
B_bank: 14 个 bank，每个 896 words x 8 bit，每个 1 BRAM18K
C_bank: 14 个 bank，每个 896 words x 32 bit，每个 2 BRAM18K
```

所以：

```text
A_bank 总 BRAM = 14
B_bank 总 BRAM = 14
C_bank 总 BRAM = 28
合计 = 56 BRAM18K
```

再加 AXI master 相关的 4 个 BRAM，顶层还是：

```text
60 BRAM18K
```

DSP 从 199 变成 200：

```text
boundary top DSP = 199
explicit top DSP = 200
```

但 `compute_block_banked` 仍然是 196 DSP，`gemm_core_mac` 也仍然是 196 DSP。这说明计算阵列没有变大；多出来的 1 个 DSP 来自 banked load/store 地址计算或除余/乘法推断一类的外围逻辑。对 ZYNQ-7020 来说：

```text
200 / 220 = 90%
```

仍然处在原来的 DSP 压力范围内。

## 这一版我学到的东西

这次结果和一开始的猜想不完全一样。

我一开始以为显式 bank 可能会继续明显减少 compute 里的 bank select mux。但实际 report 表明：

```text
真正巨大的 `mux_164_8_1_1` 已经被 boundary hoist 消掉；
显式 bank 在 boundary hoist 之后只能继续减少约 821 top LUT。
```

所以更准确的结论是：

```text
boundary hoist 是必要的大优化；
explicit banks 是有收益的小优化；
它保留 TILE=14，不增加 BRAM，但会让 load/store 的外围逻辑略微变复杂。
```

从工程取舍看，我认为 explicit banks 仍然值得保留成一个独立实验版本，因为它让顶层 LUT 从 23708 继续降到 22887，资源更松，而且没有牺牲 compute latency 或 MAC 阵列规模。

但是它不是当前最主要的性能优化方向。下一步如果继续追 latency，就不应该继续盯着这一类 bank 选择 mux，而应该看：

```text
1. load/compute/store 是否能做更稳定的数据流 overlap；
2. C tile 的 localC 累加状态是否有办法减少 32-bit mux；
3. 如果上板目标是 1008/1024，PS DDR buffer 和 AXI burst 行为是否成为新的瓶颈；
4. 如果资源还有余量，再考虑更高层的 instruction stream 或多算子调度，而不是继续微调 A/B bank。
```
