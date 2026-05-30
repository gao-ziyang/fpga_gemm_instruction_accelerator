# Combined Roofline And HLS Loop Summary

这份总表把 theoretical lower bound 和当前 HLS loop schedule 模型分开列出，避免把理想 roofline 当成 HLS latency 预测。

## Ideal lower-bound model

这是理论下界，用于判断 DDR/compute roof，不用于精确预测 HLS latency。`ideal_roofline_cycles` 是完全重叠意义下的 roofline 下界；`ideal_no_overlap_cycles` 是五阶段理想但不 overlap 的下界。

| Case | actual | ideal roofline | actual/roof | ideal no-overlap | actual/no-overlap |
| --- | --- | --- | --- | --- | --- |
| O0 | 381634 | 16384.0 | 23.29x | 82944.0 | 4.60x |
| O1 | 381634 | 16384.0 | 23.29x | 82944.0 | 4.60x |
| O2 | 317122 | 16384.0 | 19.36x | 82944.0 | 3.82x |
| O4 | 721602 | 16384.0 | 44.04x | 82944.0 | 8.70x |
| O5 | 721602 | 16384.0 | 44.04x | 82944.0 | 8.70x |
| O4inline | 2988226 | 16384.0 | 182.39x | 82944.0 | 36.03x |
| O6a | 381634 | 16384.0 | 23.29x | 82944.0 | 4.60x |
| O6b | 317186 | 16384.0 | 19.36x | 82944.0 | 3.82x |
| O1_224_generic | 381879 | 57344.0 | 6.66x | 116736.0 | 3.27x |
| O6c_fullonly_224 | 381634 | 57344.0 | 6.66x | 116736.0 | 3.27x |
| O7a | 330946 | 16384.0 | 20.20x | 82944.0 | 3.99x |
| O7b | 413890 | 16384.0 | 25.26x | 82944.0 | 4.99x |
| O8a | 705730 | 16384.0 | 43.07x | 82944.0 | 8.51x |
| O8b | 637122 | 16384.0 | 38.89x | 82944.0 | 7.68x |
| O8c | 794306 | 16384.0 | 48.48x | 82944.0 | 9.58x |

## HLS loop schedule model

这是根据当前 C++ 循环 tripcount 和 II 建的模型，更适合解释 HLS latency。它对应图里的 `T_load_AB_block + T_compute_block_internal + T_store_C_block + T_control`。

| Case | load | compute internal | store | T_control | model | actual | gap | category | deploy |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 100352 | 186368 | 50176 | 44738 | 336896 | 381634 | 1.13x | T_compute_block_internal | deployable |
| O1 | 100352 | 186368 | 50176 | 44738 | 336896 | 381634 | 1.13x | T_compute_block_internal | deployable |
| O2 | 100352 | 121856 | 50176 | 44738 | 272384 | 317122 | 1.16x | T_compute_block_internal | not deployable |
| O4 | 100352 | 473088 | 50176 | 97986 | 623616 | 721602 | 1.16x | T_compute_block_internal | not deployable |
| O5 | 100352 | 473088 | 50176 | 97986 | 623616 | 721602 | 1.16x | T_compute_block_internal | not deployable |
| O4inline | 100352 | 473088 | 50176 | 2364610 | 623616 | 2988226 | 4.79x | T_control | deployable |
| O6a | 100352 | 186368 | 50176 | 44738 | 336896 | 381634 | 1.13x | T_compute_block_internal | not deployable |
| O6b | 100352 | 121856 | 50176 | 44802 | 272384 | 317186 | 1.16x | T_compute_block_internal | not deployable |
| O1_224_generic | 100352 | 186368 | 50176 | 44983 | 336896 | 381879 | 1.13x | T_compute_block_internal | deployable |
| O6c_fullonly_224 | 100352 | 186368 | 50176 | 44738 | 336896 | 381634 | 1.13x | T_compute_block_internal | deployable |
| O7a | 100352 | 94208 | 50176 | 86210 | 244736 | 330946 | 1.35x | T_load_AB_block | not deployable |
| O7b | 100352 | 75776 | 50176 | 187586 | 226304 | 413890 | 1.83x | T_control | not deployable |
| O8a | 100352 | 874496 | 50176 | -319294 | 1025024 | 705730 | 0.69x | T_compute_block_internal | not deployable |
| O8b | 100352 | 874496 | 50176 | -387902 | 1025024 | 637122 | 0.62x | T_compute_block_internal | deployable |
| O8c | 100352 | 874496 | 50176 | -230718 | 1025024 | 794306 | 0.77x | T_compute_block_internal | not deployable |

## 结论

1. 当前 HLS loop model 最接近实际的是 `O0`，model gap 约为 `1.13x`。
2. local load/store 占每个 output tile 时间超过 50% 的版本有：O0(69.2%), O1(69.2%), O2(52.9%), O4(87.9%), O5(87.9%), O4inline(87.9%), O6a(69.2%), O6b(52.9%), O1_224_generic(69.2%), O6c_fullonly_224(69.2%), O8a(93.4%), O8b(93.4%), O8c(93.4%)。这说明主要问题不是 MAC 数量不够，而是 local feeding 没有被隐藏。
3. O1/O2/O4/O5 的差异：O1 模型为 `336896` cycles；O2 通过 row_unroll=2 把模型降到 `272384` cycles；O4/O5 的 local A/B helper 让每个 output tile 的 local load 变重，模型升到 `623616` / `623616` cycles。
4. LUT 超过 `53200` 的版本标记为 not deployable：O2, O4, O5, O6a, O6b, O7a, O7b, O8a, O8c。
5. O1 是当前 ZYNQ-7020 可落地 baseline：资源没有超过 LUT 上限，虽然 latency 还明显高于理想下界。
6. O2 虽然更快，但 LUT=67546 超过 `53200`，只能作为性能探索点，不能作为当前落地版本。
