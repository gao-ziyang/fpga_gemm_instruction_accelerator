# Ideal Lower-Bound Model

这份表由 `python/analysis/roofline_model.py` 生成，模型口径是 optimistic lower bound。

它保留外部 traffic 计算，但把 DDR、compute 和 local tile 搬运都按理想下界处理；它用于判断理论 roof，不用于精确预测当前 HLS latency。

| Case | ideal roofline | ideal no-overlap | actual latency | actual/roofline | actual/no-overlap | compute cycles | external cycles | ideal internal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 16384.0 | 82944.0 | 381634 | 23.29x | 4.60x | 10699.8 | 16384.0 | 66560 |
| O1 | 16384.0 | 82944.0 | 381634 | 23.29x | 4.60x | 10699.8 | 16384.0 | 66560 |
| O2 | 16384.0 | 82944.0 | 317122 | 19.36x | 3.82x | 10699.8 | 16384.0 | 66560 |
| O4 | 16384.0 | 82944.0 | 721602 | 44.04x | 8.70x | 10699.8 | 16384.0 | 66560 |
| O5 | 16384.0 | 82944.0 | 721602 | 44.04x | 8.70x | 10699.8 | 16384.0 | 66560 |
| O4inline | 16384.0 | 82944.0 | 2988226 | 182.39x | 36.03x | 10699.8 | 16384.0 | 66560 |
| O6a | 16384.0 | 82944.0 | 381634 | 23.29x | 4.60x | 10699.8 | 16384.0 | 66560 |
| O6b | 16384.0 | 82944.0 | 317186 | 19.36x | 3.82x | 10699.8 | 16384.0 | 66560 |
| O1_224_generic | 57344.0 | 116736.0 | 381879 | 6.66x | 3.27x | 57344.0 | 50176.0 | 66560 |
| O6c_fullonly_224 | 57344.0 | 116736.0 | 381634 | 6.66x | 3.27x | 57344.0 | 50176.0 | 66560 |
| O7a | 16384.0 | 82944.0 | 330946 | 20.20x | 3.99x | 10699.8 | 16384.0 | 66560 |
| O7b | 16384.0 | 82944.0 | 413890 | 25.26x | 4.99x | 10699.8 | 16384.0 | 66560 |
| O8a | 16384.0 | 82944.0 | 705730 | 43.07x | 8.51x | 10699.8 | 16384.0 | 66560 |

## 直接理解

1. `ideal_roofline_cycles` 是传统 roofline 下界，只取 DDR total cycles 和 compute cycles 的最大值。
2. `ideal_no_overlap_cycles` 是五阶段理想模型，不做 load/compute/store overlap，但假设 local tile 搬运已经达到理想 banking。
3. 如果实际 latency 仍远高于这两个值，说明问题主要在当前 HLS loop schedule、local feeding、控制和资源约束，而不是矩阵乘本身的理论算力。
