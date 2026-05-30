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
| O4inline | 128.000 | 0.702 | 0.36% | 0.55% | 16384.0 | 182.39x | internal/scheduler-bound |
| O6a | 128.000 | 5.495 | 2.80% | 4.29% | 16384.0 | 23.29x | internal/scheduler-bound |
| O6b | 128.000 | 6.612 | 3.37% | 5.17% | 16384.0 | 19.36x | internal/scheduler-bound |
| O1_224_generic | 196 | 29.432 | 15.02% | 15.02% | 57344.0 | 6.66x | internal/scheduler-bound |
| O6c_fullonly_224 | 196 | 29.451 | 15.03% | 15.03% | 57344.0 | 6.66x | internal/scheduler-bound |
| O7a | 128.000 | 6.337 | 3.23% | 4.95% | 16384.0 | 20.20x | internal/scheduler-bound |
| O7b | 128.000 | 5.067 | 2.59% | 3.96% | 16384.0 | 25.26x | internal/scheduler-bound |
| O8a | 128.000 | 2.972 | 1.52% | 2.32% | 16384.0 | 43.07x | internal/scheduler-bound |
| O8b | 128.000 | 3.292 | 1.68% | 2.57% | 16384.0 | 38.89x | internal/scheduler-bound |
| O8c | 128.000 | 2.640 | 1.35% | 2.06% | 16384.0 | 48.48x | internal/scheduler-bound |

## 外部 traffic 与 roof 下界

| Case | A bytes | B bytes | C bytes | external bytes | mem roof MAC/cycle | compute roof MAC/cycle |
| --- | --- | --- | --- | --- | --- | --- |
| O0 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O1 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O2 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O4 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O5 | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O4inline | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O6a | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O6b | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O1_224_generic | 100352 | 100352 | 200704 | 401408 | 224.000 | 196 |
| O6c_fullonly_224 | 100352 | 100352 | 200704 | 401408 | 224.000 | 196 |
| O7a | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O7b | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O8a | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O8b | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |
| O8c | 32768 | 32768 | 65536 | 131072 | 128.000 | 196 |

## 内部 scheduler 模型

| Case | internal model cycles | total model cycles | actual latency | local model gap | total model gap | modeled MAC active | full tiles | tail tiles |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 47600.0 | 63984.0 | 381634 | 8.02x | 5.96x | 29.41% | 64 | 136 |
| O1 | 47600.0 | 63984.0 | 381634 | 8.02x | 5.96x | 29.41% | 64 | 136 |
| O2 | 30800.0 | 47184.0 | 317122 | 10.30x | 6.72x | 45.45% | 64 | 136 |
| O4 | 117600.0 | 133984.0 | 721602 | 6.14x | 5.39x | 11.90% | 64 | 136 |
| O5 | 117600.0 | 133984.0 | 721602 | 6.14x | 5.39x | 11.90% | 64 | 136 |
| O4inline | 117600.0 | 133984.0 | 2988226 | 25.41x | 22.30x | 11.90% | 64 | 136 |
| O6a | 47600.0 | 63984.0 | 381634 | 8.02x | 5.96x | 29.41% | 64 | 136 |
| O6b | 30800.0 | 47184.0 | 317186 | 10.30x | 6.72x | 45.45% | 64 | 136 |
| O1_224_generic | 186368.0 | 236544.0 | 381879 | 2.05x | 1.61x | 30.77% | 512 | 0 |
| O6c_fullonly_224 | 186368.0 | 236544.0 | 381634 | 2.05x | 1.61x | 30.77% | 512 | 0 |
| O7a | 23600.0 | 39984.0 | 330946 | 14.02x | 8.28x | 59.32% | 64 | 136 |
| O7b | 18800.0 | 35184.0 | 413890 | 22.02x | 11.76x | 74.47% | 64 | 136 |
| O8a | 215600.0 | 231984.0 | 705730 | 3.27x | 3.04x | 6.49% | 64 | 136 |
| O8b | 215600.0 | 231984.0 | 637122 | 2.96x | 2.75x | 6.49% | 64 | 136 |
| O8c | 215600.0 | 231984.0 | 794306 | 3.68x | 3.42x | 6.49% | 64 | 136 |

## 资源效率

| Case | GOPS@freq | GOPS/DSP | GOPS/BRAM18K | GOPS/kLUT | DSP | BRAM18K | LUT |
| --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 1.099 | 0.00561 | 0.01963 | 0.02234 | 196 | 56 | 49206 |
| O1 | 1.099 | 0.00561 | 0.01963 | 0.02242 | 196 | 56 | 49023 |
| O2 | 1.323 | 0.00675 | 0.01575 | 0.01958 | 196 | 84 | 67546 |
| O4 | 0.581 | 0.00297 | 0.01038 | 0.00696 | 196 | 56 | 83514 |
| O5 | 0.581 | 0.00297 | 0.01038 | 0.00696 | 196 | 56 | 83514 |
| O4inline | 0.140 | 0.00072 | 0.00251 | 0.00508 | 196 | 56 | 27623 |
| O6a | 1.099 | 0.00280 | 0.01963 | 0.01591 | 392 | 56 | 69099 |
| O6b | 1.322 | 0.00337 | 0.01574 | 0.00982 | 392 | 84 | 134637 |
| O1_224_generic | 5.886 | 0.02958 | 0.10511 | 0.11477 | 199 | 56 | 51289 |
| O6c_fullonly_224 | 5.890 | 0.03005 | 0.10518 | 0.30146 | 196 | 56 | 19539 |
| O7a | 1.267 | 0.00647 | 0.00566 | 0.01532 | 196 | 224 | 82719 |
| O7b | 1.013 | 0.00517 | 0.00259 | 0.00525 | 196 | 392 | 193181 |
| O8a | 0.594 | 0.00303 | 0.01061 | 0.00685 | 196 | 56 | 86755 |
| O8b | 0.658 | 0.00336 | 0.01176 | 0.01371 | 196 | 56 | 48029 |
| O8c | 0.528 | 0.00269 | 0.00943 | 0.00617 | 196 | 56 | 85521 |

## 我的直接理解

1. 在 128 规模实验里，外部 roofline 当前给出的 attainable roof 是 `128 MAC/cycle`，而 O2 只有约 `6.61 MAC/cycle`；在 224 full-block 实验里，attainable roof 到 `196 MAC/cycle`，O1_224_generic/O6c 也只有约 `29.43-29.45 MAC/cycle`，所以这些点仍然归类为 `internal/scheduler-bound`。
2. 128 规模下 O2 的 `attainable_roof_util` 约为 5.17%；224 full-block 下 O1_224_generic/O6c 提升到约 15.02%-15.03%，但离 MAC 阵列真正吃满仍然很远，说明内部 scheduler 和 local feeding 还有很大优化空间。
3. 旧的 `modeled_mac_active_ratio` 只能解释局部 output tile，不能解释完整 scheduler latency；新增的 `local_model_gap` 和 `total_model_gap` 用来暴露模型没解释掉的 HLS 开销。
4. O2 绝对性能最好，但 BRAM/LUT 效率下降；O7a/O7b 继续提高 row banking 后资源明显爆炸且 latency 退化，所以行方向 banking 不适合作为落地路线。
5. O4/O5 功能通过但性能和资源效率都明显失败；O4inline 虽然 LUT 下降，但 latency 退化到接近 `3M` cycles，说明 `INLINE off` 不是唯一主因，combined local A/B load 结构本身不适合继续加码。
6. O6 runtime full-block fast path 功能通过但会把 full/fallback 双路径一起综合；O6c full-only 和 `O1_224_generic` 的干净对照说明，边界/generic 控制主要增加 LUT/FF，而不是显著增加当前调度结构下的 latency。
7. 下一步更应该保留必要的列方向 banking，转向更清楚的数据流、double buffer 和 load-compute overlap，而不是继续 row banking 或 local A/B helper 合并。
