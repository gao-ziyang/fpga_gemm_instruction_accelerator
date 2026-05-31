# Phase 2 / Iteration 018：O8b/O8c local double buffer A2，静态 ping-pong 和 ktile DATAFLOW

这次接着 O8a 往路线 A 走一步，尝试让 local double buffer 不只是顺序 ping-pong，而是用更静态的 ping/pong buffer 和局部 DATAFLOW 去靠近 `load next + compute current` 的重叠。

## 目标

O8a 已经证明动态 `localA_bank[bank]` / `localB_bank[bank]` 写法会让 local load 的 Final II 退化到 7。A2 的目标是先去掉动态 bank index，再尝试让 HLS 看到更明确的两套 local buffer：

```text
localA_ping/localB_ping
localA_pong/localB_pong
```

理想上每个 K tile 想从：

```text
T_A_load + T_B_load + T_compute = 14 + 14 + 14 = 42 cycles
```

靠近：

```text
max(T_A_load + T_B_load, T_compute) = 28 cycles
```

这次做了两个小实验：

```text
O8b: static ping/pong helper，尝试块内 DATAFLOW，但 HLS 没有接受块级 DATAFLOW。
O8c: 把 load next + compute current 拆成函数，让函数级 DATAFLOW 真正被 HLS 接受。
```

## 修改

新增脚本：

```text
hls/scripts/run_hls_accel_log18_o8b_static_double_buffer.tcl
hls/scripts/run_hls_accel_log19_o8c_ktile_dataflow.tcl
```

新增或扩展的 helper：

```text
load_local_a_tile_static()
load_local_b_tile_static()
load_local_a_tile_kt()
load_local_b_tile_kt()
load_next_and_compute_kt()
```

宏配置：

```text
GZY_ACCEL_LOAD_AB_PARALLEL=1
GZY_ACCEL_LOCAL_ROW_UNROLL=1
GZY_ACCEL_LOCAL_DOUBLE_BUFFER=2   # O8b
GZY_ACCEL_LOCAL_DOUBLE_BUFFER=3   # O8c
```

O1 默认仍然是：

```text
GZY_ACCEL_LOCAL_DOUBLE_BUFFER=0
```

所以这次不会破坏 O1 baseline。

## 实验结果

| Case | 宏配置 | C-sim | C-synth | Cosim | Latency | BRAM18K | DSP | FF | LUT | Estimated clock | MAC/cycle | LUT<53200 | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O1 | loadAB=1,row_unroll=1 | PASS | PASS | PASS | 381634 | 56 | 196 | 33282 | 49023 | 7.165 ns | 5.495 | yes | 当前可落地 baseline |
| O8a | O1 + local_double_buffer=1 | PASS | PASS | PASS | 705730 | 56 | 196 | 46991 | 86755 | 7.165 ns | 2.972 | no | 动态 bank ping-pong 失败 |
| O8b | O1 + local_double_buffer=2 | PASS | PASS | PASS | 637122 | 56 | 196 | 56031 | 48029 | 7.165 ns | 3.292 | yes | 资源过线内，但 latency 明显慢于 O1 |
| O8c | O1 + local_double_buffer=3 | PASS | PASS | FAIL/stop | 794306 csynth est. | 56 | 196 | 87153 | 85521 | 7.165 ns | 2.640 | no | DATAFLOW 被接受但资源爆炸 |

O8b 是一个比较有意思的结果：LUT 没超过 53200，DSP 也保持 196，但 latency 退化到 `637122 cycles`，所以虽然资源看起来可部署，性能上不是优化。

O8c 证明函数级 DATAFLOW 确实能被 Vitis HLS 2020.2 接受，但结果很不理想。`load_next_and_compute_kt()` 单个 dataflow helper 就达到 `56968 LUT`，top 汇总为 `85521 LUT`，已经超过 ZYNQ-7020 上限。RTL cosim 长时间停在 0% 附近，我中止了仿真，因此记录为 FAIL/stop；综合报告已经足够判断这一路线不可落地。

## 关键 loop II

| Loop / Function | O8b Final II / Latency | O8c Final II / Latency | 说明 |
| --- | --- | --- | --- |
| `load_ab_k_load_ab_x` | II=1 | II=1 | 外层 A/B block load 保持正常 |
| `load_local_a_static_group` | II=7 | - | O8b 静态 ping helper 仍然有端口/寻址冲突 |
| `load_local_b_static_group` | II=7 | - | O8b 没有救回 local B load |
| `load_local_a_kt_group` | - | II=7 | O8c dataflow process 内仍退化 |
| `load_local_b_kt_group` | - | II=7 | B 方向同样退化 |
| `core_k_loop` | II=1 | II=1 | MAC 阵列本身没有坏 |
| `load_next_and_compute_kt` | - | latency=137, dataflow | 函数级 DATAFLOW 生效，但 FIFO/LUT 代价太大 |

O8c 的 HLS 日志里有：

```text
Applying dataflow to function 'load_next_and_compute_kt'
detected/extracted 4 process function(s)
```

同时也有三条重要 warning：

```text
Process load_local_a_tile_kt7 has both a predecessor and reads an input from its caller.
Process load_local_b_tile_kt8 has both a predecessor and reads an input from its caller.
Process gemm_core_mac has both a predecessor and reads an input from its caller.
```

我的理解是：HLS 虽然把函数拆成 dataflow process，但这些 process 还直接读 caller 传进来的大数组/标量，导致它需要插入大量握手、FIFO 和 mux。`load_next_and_compute_kt()` 的 FIFO 项 alone 就是：

```text
FIFO: 39600 FF, 27200 LUT
```

这已经超过了 O1 留给优化的 LUT 空间。

## Latency 分解

按当前 loop schedule 模型，O8b/O8c 都可以先用 local load II=7 粗估：

```text
T_load_AB_block              = 100352
T_compute_block_internal     = 874496
T_store_C_block              = 50176
T_hls_loop_model             = 1025024
```

对应实际或综合估计：

| Case | HLS loop model | HLS latency | unexplained | 说明 |
| --- | --- | --- | --- | --- |
| O8b | 1025024 | 637122 RTL | -387902 | 静态函数调度让简单模型偏保守，但实际仍慢于 O1 |
| O8c | 1025024 | 794306 csynth est. | -230718 | 局部 DATAFLOW overlap 了一部分，但资源和总 latency 仍不可接受 |

和理想下界相比：

| Case | latency / ideal roofline | latency / ideal no-overlap | 说明 |
| --- | --- | --- | --- |
| O1 | 23.29x | 4.60x | baseline 的主要差距来自内部 feeding/control |
| O8b | 38.89x | 7.68x | double buffer 没有转化成有效稳态流水 |
| O8c | 48.48x | 9.58x | DATAFLOW 付出的调度/FIFO 代价超过 overlap 收益 |

## 为什么 A2 仍然失败

1. O8b 虽然不用动态 bank 数组，但 helper 的 `tk` 仍是运行时参数，HLS 不能稳定证明 `tk` 总是 `14` 的倍数，所以 bank/partition 访存没有恢复到 O1 那种简单模式。
2. O8b 里块级 `#pragma HLS DATAFLOW` 没有被有效接受，只是普通顺序执行，所以不会形成预取和计算的真实 overlap。
3. O8c 改成函数级 DATAFLOW 后，HLS 确实接受了，但 process 之间传 complete-partition 的 local array 时插入了大量 FIFO。
4. `gemm_core_mac()` 仍保持 196 DSP 和 II=1，说明不是计算阵列本身坏了；真正坏在 local feeding 函数边界和 dataflow 通道代价。
5. 当前 O1 已经用到 49023 LUT，只剩约 4177 LUT 余量。O8c 的 dataflow helper 单独超过 56000 LUT，不可能在 ZYNQ-7020 上落地。

## 结论

O8b/O8c 都没有得到一个 `LUT<53200 且 latency 明显优于 O1` 的版本。O8b 资源在 LUT 线内，但 latency 比 O1 慢很多；O8c 有 DATAFLOW 迹象，但资源直接爆炸。

所以当前 best deployable baseline 仍然是 O1。double buffering 这个想法本身仍然有意义，但在当前 `TILE=14,BLOCK=112`、complete partition local array、helper/dataflow over array 的写法下，不适合作为小步可落地优化继续推进。

如果后面还想碰路线 A，我更倾向于换成更底层的 local feeding 重写，例如让 B 的存储布局和读取维度更匹配，或者做一个很小规模的 streaming micro-architecture prototype，而不是继续在 O1 结构上套 helper/dataflow。
