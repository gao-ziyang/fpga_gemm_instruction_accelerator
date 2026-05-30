# Iteration 016: O8 local double buffer A1

这次只做路线 A 的最小实验，不动 O1 baseline，也不直接实现路线 D 的 block-level DATAFLOW。

## 目标

O1 里每个 K tile 大致是：

```text
T_A_load = 14 cycles
T_B_load = 14 cycles
T_compute = 14 cycles
```

理想的 local double buffering 想把下一块 localA/localB 的搬运和当前 `gemm_core_mac()` 重叠，目标从：

```text
14 + 14 + 14 = 42 cycles / K tile
```

变成接近：

```text
max(14 + 14, 14) = 28 cycles / K tile
```

这次先做 A1：顺序 ping-pong 版本，用来确认功能、资源和 HLS scheduler 行为。没有加 `DATAFLOW`，所以不期待一定产生真实 overlap。

## 修改

新增宏：

```cpp
#ifndef GZY_ACCEL_LOCAL_DOUBLE_BUFFER
#define GZY_ACCEL_LOCAL_DOUBLE_BUFFER 0
#endif

#ifndef GZY_ACCEL_DATAFLOW_BLOCK_OVERLAP
#define GZY_ACCEL_DATAFLOW_BLOCK_OVERLAP 0
#endif
```

在 `compute_block()` 中增加：

```cpp
#if GZY_ACCEL_LOCAL_DOUBLE_BUFFER
    gemm_data_t localA_bank[2][GEMM_TILE][GEMM_TILE];
    gemm_data_t localB_bank[2][GEMM_TILE][GEMM_TILE];
    // preload ping, then alternate ping/pong
#else
    // original O1/O2 path
#endif
```

新增 helper：

```text
load_local_a_bank()
load_local_b_bank()
```

Tcl 配置为 O1 baseline 加一个变量：

```text
GZY_ACCEL_LOAD_AB_PARALLEL=1
GZY_ACCEL_LOCAL_ROW_UNROLL=1
GZY_ACCEL_LOCAL_DOUBLE_BUFFER=1
```

脚本：

```text
hls/scripts/run_hls_accel_log17_o8_local_double_buffer.tcl
```

## 实验结果

| Case | 宏配置 | C-sim | C-synth | Cosim | Latency | BRAM18K | DSP | FF | LUT | Estimated clock | MAC/cycle | LUT<53200 | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O1 | loadAB=1,row_unroll=1 | PASS | PASS | PASS | 381634 | 56 | 196 | 33282 | 49023 | 7.165 ns | 5.495 | yes | 当前可落地 baseline |
| O8a | O1 + local_double_buffer=1 | PASS | PASS | PASS | 705730 | 56 | 196 | 46991 | 86755 | 7.165 ns | 2.972 | no | exploration only，失败 |

O8a 的 DSP 仍是 196，没有复制 MAC 阵列；但是 LUT 从 O1 的 `49023` 增加到 `86755`，已经远超 ZYNQ-7020 的 `53200` 上限。latency 也从 `381634` 退化到 `705730`。

## 关键 loop II

| Loop | O8a Final II | 说明 |
| --- | --- | --- |
| `load_ab_k_load_ab_x` | 1 | 外层 A/B block 合并加载保持正常 |
| `load_local_a_db_group` | 7 | 新 helper 读 `A_buf` 时出现 memory port 限制 |
| `load_local_b_db_group` | 7 | 新 helper 读 `B_buf` 时出现 memory port 限制 |
| `core_k_loop` | 1 | MAC 阵列本身仍然保持 II=1 |
| `store_c_i_store_c_j` | 1 | C 写回保持正常 |

HLS 还生成了 `mux_1432_8_1_1` 等大 mux，`load_local_a_bank/load_local_b_bank` 各自约 `9753 LUT`。这说明动态 `bank` 选择没有被 HLS 当成简单的 ping-pong 静态路径，而是变成了额外寻址/mux/端口仲裁。

## Latency 分解

用当前 HLS loop schedule 模型粗分：

```text
T_load_AB_block              = 100352
T_compute_block_internal     = 874496
T_store_C_block              = 50176
T_hls_loop_model             = 1025024
HLS actual                   = 705730
T_unexplained                = -319294
```

这里模型大于实际，说明 O8a 的 helper/function 级调度已经不再被简单 `tripcount x II` 完整解释。这个负 gap 仍然保留，不截断。即使按更乐观的实际 cosim latency 看，O8a 也明显慢于 O1。

和理想下界的差距：

```text
ideal_roofline_cycles        = 16384
ideal_no_overlap_cycles      = 82944
O8a actual / ideal_roofline  = 43.07x
O8a actual / ideal_no_overlap= 8.51x
```

O8a 的每个 output tile 里 local load/store 估计占比约 `93.44%`。这说明 A1 没有隐藏 local feeding，反而让 local feeding 更重。

## 为什么 A1 失败

1. A1 只是顺序 ping-pong，没有 `DATAFLOW`，所以 HLS 没有理由把 load next 和 compute current 自动重叠成稳态流水。
2. `localA_bank[bank]` / `localB_bank[bank]` 使用动态 bank index，HLS 生成大 mux，而不是两个静态可分离的 ping/pong 数据通路。
3. `load_local_a_bank/load_local_b_bank` 是新的函数边界，HLS 对 `A_buf/B_buf` 端口依赖更保守，Final II 退化到 7。
4. MAC 阵列没有复制是好事，但 local load helper 的 LUT 和调度代价已经超过了收益空间。

所以 O8a 是一个有价值的反例：local double buffer 方向不一定错，但这种动态 bank helper 写法不适合作为可落地路线。

## 路线 D 只做分析

当前 scheduler 可以考虑三个 overlap 层级：

| 层级 | 可 overlap 内容 | 风险 |
| --- | --- | --- |
| K tile 级 | load next localA/localB 与 compute current tile | 最贴近瓶颈，但需要静态 ping/pong 或局部 dataflow；容易产生 mux、端口冲突或函数调度失败 |
| K block 级 | load next `A_buf/B_buf` 与 compute current block | 对 `C_buf` 有跨 K block 累加 RAW 依赖，store 必须等最后一个 K block |
| N/M block 级 | load next output block、compute current、store previous | 需要 A/B/C block buffer ping-pong，BRAM 和控制都会增加 |

`A_buf/B_buf` 本身没有跨 block 的 RAW 依赖，可以做 ping-pong；`C_buf` 有更强约束：

```text
reset_c=true 的第一个 k0 block 写 C_buf
后续 k0 block 读旧 C_buf 再累加
最后一个 k0 block 后才能 store C_buf
```

所以如果做 block-level DATAFLOW，`C_buf` 要么也 ping-pong，要么必须严格分阶段，避免 store previous 和 compute current 访问同一个 C buffer。

资源预估上，当前 O1 已经 `56 BRAM18K / 49023 LUT`。如果完整复制 A/B/C block buffer，BRAM 可能接近翻倍；更危险的是 DATAFLOW 控制、FIFO、mux 和边界判断会继续推高 LUT。O8a 只加 local ping-pong helper 就到 `86755 LUT`，说明直接在 `TILE=14,BLOCK=112` 上做完整 D 路线，很难保持 `LUT<53200`。

因此路线 D 暂时不直接大改。更合理的下一步是先做小规模 prototype 或者重新写一个静态 ping/pong A2：

```text
load ping with a dedicated function
compute ping while loading pong only if HLS report shows real DATAFLOW overlap
avoid dynamic bank index in inner helper
keep DSP at 196
stop immediately if LUT crosses 53200
```

## 结论

O8a 功能正确，C-sim/C-synth/C/RTL cosim 都通过；但它不是可落地优化。当前 best deployable baseline 仍然是 O1。O2 更快但 LUT 超预算，O8a 则同时 latency 退化和 LUT 超预算。
