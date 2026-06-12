# Phase 2 汇总：TILE=14 GEMM scheduler 优化

Phase 2 的目标是把 GEMM 从“能算对”推进到“能作为真正 IP 核评价性能”。这一阶段主要围绕 `TILE=14`、`BLOCK_N/K/M=112`，尝试优化 load AB、row banking、local AB、full-block fast path 和 block-level dataflow。

## 主配置

代表性配置：

```text
GZY_GEMM_TILE     = 14
GZY_ACCEL_BLOCK_N = 112
GZY_ACCEL_BLOCK_K = 112
GZY_ACCEL_BLOCK_M = 112
```

这里 `14*14=196` 个 MAC 基本吃满 ZYNQ-7020 的 DSP 预算，所以它适合作为 GEMM-only 性能 IP 的主要路线。

## O0-O5 结果

固定 `128^3` cosim/综合对照中，比较有代表性的结果是：

| 版本 | 主要变化 | BRAM | DSP | FF | LUT | latency | MAC/cycle |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| O0 | serial load | 56 | 196 | 33470 | 49206 | 381634 | 5.50 |
| O1 | combined A/B block load | 56 | 196 | 33282 | 49023 | 381634 | 5.50 |
| O2 | row banking = 2 | 84 | 196 | 34317 | 67546 | 317122 | 6.61 |
| O3 | local A/B merged loop | 56 | 196 | 43062 | 27623 | 2979010 | 0.70 |
| O4 | helper 封装 | 56 | 196 | 40296 | 83514 | 721602 | 2.91 |
| O5 | helper formal partition | 56 | 196 | 40296 | 83514 | 721602 | 2.91 |

这组结果说明：

1. O2 的 row banking 确实能降低 latency，但 BRAM/LUT 成本高。
2. 把代码封装成 helper 不等于硬件更好，反而可能让 HLS 生成更差的结构。
3. O3/O4/O5 都不是稳定主线，后面没有继续作为上板基线。

## Roofline 口径

当时的估算：

```text
compute roof 约 196 MAC/cycle
external memory roof 约 128 MAC/cycle
O1 实测约 5.50 MAC/cycle
O2 实测约 6.61 MAC/cycle
```

所以瓶颈不在理论 MAC 阵列，也不简单是 DDR 带宽，而是在 scheduler 内部的 load/compute/store 调度、片上 buffer 组织、循环边界和 HLS 生成结构。

## 为什么 128 不一定比 112 好

`112` 正好是 `BLOCK=112` 的完整块；`128` 会拆成 `2x2x2` 个 block，其中很多 block 是 partial block。对当前 scheduler 来说，这会多出大量边界判断、块调度和 store mask 相关开销。因此小尺寸下 `128` 可能比 `112` 的 MAC/cycle 低很多。

这个现象不是数学上“矩阵越大越慢”，而是当前 block 大小和矩阵尺寸不对齐导致的。

## 阶段结论

Phase 2 最重要的结论是：`TILE=14` 能把 GEMM MAC 阵列做得很强，但必须让 scheduler 结构足够简单、片上 buffer 足够明确，才能真正转成有效吞吐。最后保留下来的方向不是 O2/O4/O5，而是后面 Phase 3 的 boundary hoist 和 explicit banks。
