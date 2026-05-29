# Internal Roofline Summary

这份表由 `python/analysis/roofline_model.py` 生成，实验点来自 `python/analysis/roofline_experiments.csv`。

当前默认假设：`freq=100MHz`，`DDR effective bandwidth=800MB/s`，即 `8 bytes/cycle`；计算口径同时保留 MAC/cycle 和 ops/cycle，避免单位混用。

## Roofline 分类

| Case | attainable MAC/cycle | actual MAC/cycle | compute util | attainable util | roof cycles min | latency/roof | bound |
| --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 128.000 | 5.495 | 2.80% | 4.29% | 16384.0 | 23.29x | internal/scheduler-bound |
| O1 | 128.000 | 5.495 | 2.80% | 4.29% | 16384.0 | 23.29x | internal/scheduler-bound |
| O2 | 128.000 | 6.613 | 3.37% | 5.17% | 16384.0 | 19.36x | internal/scheduler-bound |
| O4 | 128.000 | 2.906 | 1.48% | 2.27% | 16384.0 | 44.04x | internal/scheduler-bound |
| O5 | 128.000 | 2.906 | 1.48% | 2.27% | 16384.0 | 44.04x | internal/scheduler-bound |

## 外部 traffic 与 roof 下界

| Case | A bytes | B bytes | C bytes | external bytes | mem roof MAC/cycle | compute roof MAC/cycle |
| --- | --- | --- | --- | --- | --- | --- |
| O0 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O1 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O2 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O4 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O5 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |

## 内部 scheduler 模型

| Case | internal model cycles | total model cycles | actual latency | local model gap | total model gap | modeled MAC active | full tiles | tail tiles |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 47600.0 | 63984.0 | 381634 | 8.02x | 5.96x | 29.41% | 64 | 136 |
| O1 | 47600.0 | 63984.0 | 381634 | 8.02x | 5.96x | 29.41% | 64 | 136 |
| O2 | 30800.0 | 47184.0 | 317122 | 10.30x | 6.72x | 45.45% | 64 | 136 |
| O4 | 117600.0 | 133984.0 | 721602 | 6.14x | 5.39x | 11.90% | 64 | 136 |
| O5 | 117600.0 | 133984.0 | 721602 | 6.14x | 5.39x | 11.90% | 64 | 136 |

## 资源效率

| Case | GOPS@freq | GOPS/DSP | GOPS/BRAM18K | GOPS/kLUT | DSP | BRAM18K | LUT |
| --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 1.099 | 0.00561 | 0.01963 | 0.02234 | 196 | 56 | 49206 |
| O1 | 1.099 | 0.00561 | 0.01963 | 0.02242 | 196 | 56 | 49023 |
| O2 | 1.323 | 0.00675 | 0.01575 | 0.01958 | 196 | 84 | 67546 |
| O4 | 0.581 | 0.00297 | 0.01038 | 0.00696 | 196 | 56 | 83514 |
| O5 | 0.581 | 0.00297 | 0.01038 | 0.00696 | 196 | 56 | 83514 |

## 我的直接理解

1. 外部 roofline 当前给出的 attainable roof 是 `128 MAC/cycle`，而 O2 只有 `6.613 MAC/cycle`，所以 O0-O5 都应该归类为 `internal/scheduler-bound`。
2. O2 的 `attainable_roof_util` 约为 5.17%，`compute_peak_util` 约为 3.37%，说明内部 scheduler 和 local feeding 还有很大优化空间。
3. 旧的 `modeled_mac_active_ratio` 只能解释局部 output tile，不能解释完整 scheduler latency；新增的 `local_model_gap` 和 `total_model_gap` 用来暴露模型没解释掉的 HLS 开销。
4. O2 绝对性能最好，但 BRAM/LUT 效率下降；O4/O5 功能通过但性能和资源效率都明显失败。
5. 下一步更应该做 full-block fast path、边界判断剥离、地址/mux 简化和更细的 TILE/BLOCK/ROW_UNROLL sweep，而不是继续粗暴合并 helper。
