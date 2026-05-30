# HLS Loop Schedule Model

这份表按当前 HLS C++ 的 loop tripcount 和 II 估算 latency，不用 `bytes / DDR bandwidth` 去估当前 memory 版 load/store。

模型形式对应：`T_total ~= T_load_AB_block + T_compute_block_internal + T_store_C_block + T_control`。

| Case | load | compute internal | store | model total | actual | unexplained | gap | category | deploy |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 100352 | 186368 | 50176 | 336896 | 381634 | 44738 | 1.13x | T_compute_block_internal | deployable |
| O1 | 100352 | 186368 | 50176 | 336896 | 381634 | 44738 | 1.13x | T_compute_block_internal | deployable |
| O2 | 100352 | 121856 | 50176 | 272384 | 317122 | 44738 | 1.16x | T_compute_block_internal | not deployable |
| O4 | 100352 | 473088 | 50176 | 623616 | 721602 | 97986 | 1.16x | T_compute_block_internal | not deployable |
| O5 | 100352 | 473088 | 50176 | 623616 | 721602 | 97986 | 1.16x | T_compute_block_internal | not deployable |
| O4inline | 100352 | 473088 | 50176 | 623616 | 2988226 | 2364610 | 4.79x | T_control | deployable |
| O6a | 100352 | 186368 | 50176 | 336896 | 381634 | 44738 | 1.13x | T_compute_block_internal | not deployable |
| O6b | 100352 | 121856 | 50176 | 272384 | 317186 | 44802 | 1.16x | T_compute_block_internal | not deployable |
| O1_224_generic | 100352 | 186368 | 50176 | 336896 | 381879 | 44983 | 1.13x | T_compute_block_internal | deployable |
| O6c_fullonly_224 | 100352 | 186368 | 50176 | 336896 | 381634 | 44738 | 1.13x | T_compute_block_internal | deployable |
| O7a | 100352 | 94208 | 50176 | 244736 | 330946 | 86210 | 1.35x | T_load_AB_block | not deployable |
| O7b | 100352 | 75776 | 50176 | 226304 | 413890 | 187586 | 1.83x | T_control | not deployable |
| O8a | 100352 | 874496 | 50176 | 1025024 | 705730 | -319294 | 0.69x | T_compute_block_internal | not deployable |
| O8b | 100352 | 874496 | 50176 | 1025024 | 637122 | -387902 | 0.62x | T_compute_block_internal | deployable |
| O8c | 100352 | 874496 | 50176 | 1025024 | 794306 | -230718 | 0.77x | T_compute_block_internal | not deployable |

## Loop Breakdown

| Case | n/m/k blk | tile n/m/k | T_load/block | T_one_tile | C read | A load | B load | AB load | compute | C write | local share |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | 2/2/2 | 8/8/8 | 12544 | 364 | 14 | 14 | 14 | 0 | 14 | 14 | 69.23% |
| O1 | 2/2/2 | 8/8/8 | 12544 | 364 | 14 | 14 | 14 | 0 | 14 | 14 | 69.23% |
| O2 | 2/2/2 | 8/8/8 | 12544 | 238 | 7 | 7 | 7 | 0 | 14 | 7 | 52.94% |
| O4 | 2/2/2 | 8/8/8 | 12544 | 924 | 14 | 14 | 14 | 98 | 14 | 14 | 87.88% |
| O5 | 2/2/2 | 8/8/8 | 12544 | 924 | 14 | 14 | 14 | 98 | 14 | 14 | 87.88% |
| O4inline | 2/2/2 | 8/8/8 | 12544 | 924 | 14 | 14 | 14 | 98 | 14 | 14 | 87.88% |
| O6a | 2/2/2 | 8/8/8 | 12544 | 364 | 14 | 14 | 14 | 0 | 14 | 14 | 69.23% |
| O6b | 2/2/2 | 8/8/8 | 12544 | 238 | 7 | 7 | 7 | 0 | 14 | 7 | 52.94% |
| O1_224_generic | 2/2/2 | 8/8/8 | 12544 | 364 | 14 | 14 | 14 | 0 | 14 | 14 | 69.23% |
| O6c_fullonly_224 | 2/2/2 | 8/8/8 | 12544 | 364 | 14 | 14 | 14 | 0 | 14 | 14 | 69.23% |
| O7a | 2/2/2 | 8/8/8 | 12544 | 184 | 4 | 4 | 4 | 0 | 14 | 4 | 39.13% |
| O7b | 2/2/2 | 8/8/8 | 12544 | 148 | 2 | 2 | 2 | 0 | 14 | 2 | 24.32% |
| O8a | 2/2/2 | 8/8/8 | 12544 | 1708 | 14 | 98 | 98 | 0 | 14 | 14 | 93.44% |
| O8b | 2/2/2 | 8/8/8 | 12544 | 1708 | 14 | 98 | 98 | 0 | 14 | 14 | 93.44% |
| O8c | 2/2/2 | 8/8/8 | 12544 | 1708 | 14 | 98 | 98 | 0 | 14 | 14 | 93.44% |
