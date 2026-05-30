# Iteration 018: Route D block-level DATAFLOW design analysis

这次不继续优化 O8，也不在 `TILE=14/BLOCK=112` 主线里直接实现完整 DATAFLOW。O8b/O8c 已经给出一个很重要的边界结论：在当前 complete-partition local tile 和 ZYNQ-7020 LUT<53200 的约束下，不能靠 `helper/DATAFLOW over local arrays` 实现可落地的 local double buffering。

所以这一版只做路线 D 的静态分析。

## 当前 O1 主循环结构

当前 `gemm_scheduler()` 的结构可以简化成：

```text
for n0:
  for m0:
    for k0:
      load A/B block -> A_buf/B_buf
      compute_block(A_buf, B_buf, C_buf, reset_c)
    store C_buf -> C_mem
```

其中 `C_buf` 在一个 `(n0,m0)` output block 内跨所有 `k0` block 累加：

```text
k0 = 0: reset_c = true,  C_buf = A0*B0
k0 > 0: reset_c = false, C_buf += Ak*Bk
最后: store C_buf
```

这个 RAW 依赖是路线 D 最关键的约束。

## 1. block-level overlap 是否必须复制 A/B/C buffer

答案取决于想 overlap 到哪一层。

| 目标 | 是否需要复制 A_buf/B_buf | 是否需要复制 C_buf | 原因 |
| --- | --- | --- | --- |
| load next A/B block + compute current block | 基本需要 | 不一定 | `load_ab_block()` 写 A/B，`compute_block()` 读 A/B；如果共用同一组 BRAM，读写同一 block buffer 会有 RAW/WAW 和端口冲突。 |
| compute current output block + store previous output block | 不需要复制 A/B | 基本需要 | `store_c_block()` 读 C_buf，`compute_block()` 对当前 output block 读写 C_buf；如果共用同一 C_buf，会把前一块结果和当前累加混在一起。 |
| load next + compute current + store previous 三阶段稳态 | 需要 | 需要 | A/B 至少 ping-pong，C 也至少 ping-pong，否则 store 和 compute 不能同时使用不同 output block 的 C。 |
| 只在同一个 `(n0,m0)` 内 overlap 下一个 `k0` 的 A/B load 和当前 `k0` compute | 需要 A/B ping-pong | 单 C_buf 可以 | C_buf 本来就是当前 output block 的累加状态，不能拆成多个 partial sum，除非额外做 partial C merge。 |

所以，如果路线 D 指的是完整的：

```text
load next A/B block
compute current block
store previous C block
```

那就不能只加 DATAFLOW pragma。至少需要：

```text
A_buf_ping / A_buf_pong
B_buf_ping / B_buf_pong
C_buf_ping / C_buf_pong
```

如果只做较保守的 load/compute overlap，可以先只复制 A/B，不复制 C，但那不是完整三阶段 DATAFLOW。

## 2. BRAM/LUT 预计增加

O1 的 HLS report 里，片上 block buffer 的 BRAM 分布是：

```text
A_buf: 14 banks x 1 BRAM18K = 14
B_buf: 14 banks x 1 BRAM18K = 14
C_buf: 14 banks x 2 BRAM18K = 28
Total: 56 BRAM18K
```

按这个估算：

| 方案 | BRAM18K 估计 | 比 O1 增加 | 说明 |
| --- | --- | --- | --- |
| O1 single buffer | 56 | 0 | 当前 baseline |
| 只复制 A/B | 84 | +28 | 可尝试 load/compute overlap，但不支持 store/compute overlap |
| 只复制 C | 84 | +28 | 只解决 store previous 和 compute current 的 C 冲突 |
| 复制 A/B/C | 112 | +56 | 才比较接近完整 load/compute/store 三阶段 overlap |

从 BRAM 数量看，ZYNQ-7020 有 280 个 BRAM18K，`112/280 = 40%`，BRAM 本身不是最先爆掉的资源。

真正危险的是 LUT。O1 已经是：

```text
LUT = 49023
ZYNQ-7020 limit ~= 53200
剩余余量 ~= 4177 LUT
```

完整 D 路线会额外带来：

```text
ping/pong buffer select
banked BRAM address/ce/we mux
DATAFLOW task start/done/ready 控制
可能的 FIFO / scalar propagation FIFO
block state machine
边界 current_N/current_M/current_K 的跨 task 传递
```

所以即使 BRAM 可以承受，LUT 很可能承受不了。O8c 已经是一个很强的警告：只是 K tile 级函数 DATAFLOW，就因为 complete-partition local array 和 task 间通信生成了大量 FIFO，使 top LUT 到 `85521`。block-level DATAFLOW 不一定会复制 O8c 的 local-array FIFO，但只要 HLS 对 banked memory/task 边界处理保守，就很容易超过 O1 剩下的约 4k LUT 空间。

我的判断是：

```text
完整 A/B/C ping-pong + DATAFLOW 在 TILE=14/BLOCK=112 下，大概率 LUT>53200。
```

## 3. 是否会让 complete-partition local array 跨 DATAFLOW task 传递

如果 D 路线放在 block level，并保持：

```text
load_ab_block_task()    // 只写 A_buf/B_buf
compute_block_task()    // 内部自己创建 localA/localB/localC
store_c_block_task()    // 只读 C_buf
```

那么理论上不会把 `localA/localB/localC` 这些 complete-partition local array 跨 DATAFLOW task 传递。它们仍然留在 `compute_block()` 内部。

这点和 O8c 不一样。O8c 的问题是把：

```text
localA_next/localB_next/localA_curr/localB_curr/localC
```

作为函数级 DATAFLOW task 的参数或跨 process 数据使用，HLS 插入了大量 FIFO 和控制逻辑。

所以 D 路线如果将来做 prototype，必须遵守一个边界：

```text
DATAFLOW task 之间只传 block buffer token / ping-pong id / scalar metadata；
不要把 complete-partition local tile array 作为 task 间数据通路。
```

但是即使不跨 local array，A_buf/B_buf/C_buf 本身也是高度 banked 的 BRAM。HLS 是否能把 ping-pong banked BRAM 当成清晰的双缓冲，而不是生成大量 mux，还必须通过小规模 report 验证。

## 4. LUT<53200 下是否可能成立

我现在的判断偏保守：

```text
在主配置 TILE=14/BLOCK=112 下，完整路线 D 很难在 LUT<53200 下成立。
```

原因有三点：

1. O1 已经用到 `49023 LUT`，只剩约 `4177 LUT`。
2. 完整 D 至少要引入 A/B ping-pong，若还想 overlap store，就要 C ping-pong；这些都会引入 bank select 和控制 mux。
3. O8c 已经说明 Vitis HLS 2020.2 对这类 DATAFLOW task 边界可能非常保守，FIFO/控制开销不是小常数。

可能成立的较小版本只有两类：

| 版本 | 是否可能 LUT<53200 | 价值 |
| --- | --- | --- |
| 只做 A/B ping-pong 的 load/compute overlap | 有一点可能，但风险仍高 | 可以验证外层 load 是否能被隐藏，但不能隐藏 store |
| 小规模 block-level DATAFLOW prototype | 可能 | 用来读 dataflow report 和资源趋势，不作为主线结果 |
| 完整 A/B/C ping-pong 三阶段 DATAFLOW | 主配置下大概率不成立 | 资源风险太高，不能直接上主线 |

因此路线 D 不应该直接进入主配置实现。它现在更适合作为设计分析和小规模验证题。

## 5. 如果要 prototype，应该怎么限制

如果后面真的要做 prototype，我建议只允许小配置，例如：

```text
TILE = 4 或 8
BLOCK_N/K/M = 16 或 32
N/K/M = 32 或 64
GZY_ACCEL_LOCAL_DOUBLE_BUFFER = 0
不启用 O8 helper/dataflow over local arrays
```

prototype 的目标不是追求 latency 数字，而是看 HLS report：

```text
1. 是否出现真实 DATAFLOW region；
2. load/compute/store 是否在 report 里成为并行 process；
3. 是否生成大量 FIFO；
4. A/B/C ping-pong buffer 是否只是 BRAM 增加，而不是 LUT 爆炸；
5. DSP 是否仍保持单套 MAC 阵列，没有变成 392；
6. 能否在小配置下 C-sim PASS、C-synth 完成。
```

不允许第一步就在：

```text
TILE=14
BLOCK_N/K/M=112
```

主线配置上实现完整 block-level DATAFLOW。

## 这轮收敛后的路线图

当前优化链条可以这样收束：

| 版本 | 角色 | 结论 |
| --- | --- | --- |
| O1 | 最终可落地 baseline | LUT=49023，DSP=196，latency=381634 |
| O2 | 性能探索点 | row banking 有收益，但 LUT=67546 超预算 |
| O3/O4/O5 | 失败路线 | 硬合并 local A/B load 导致 II/LUT/latency 退化 |
| O6 | 失败路线 | runtime full/generic 双路径复制硬件，资源爆炸 |
| O8b/O8c | 失败路线 | static ping/pong 无 overlap；ktile DATAFLOW FIFO/控制过重 |
| D | 暂不实现主线 | 只做静态分析或小规模 prototype |

这不是坏结果。现在已经排除了几条看起来合理、但在 ZYNQ-7020 上不落地的路线。后续报告里应该明确写：O1 是当前可落地版本；O2/O8 是解释瓶颈和探索上限的证据；路线 D 需要先小规模验证 HLS dataflow report，不能贸然进入主配置。
