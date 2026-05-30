# Iteration 013：O6 full-block fast path 验证

## 我这一版想解决什么

上一版 roofline 建模以后，我的判断是：现在不是外部 DDR 上限先卡住，而是 `A_buf/B_buf/C_buf -> localA/localB/localC -> MAC array` 这一段内部 scheduler 很低效。O2 通过 row banking 把 latency 从 `381634` 降到 `317122`，说明 local feeding 的方向是对的；但 O4/O5 的 helper 合并方向又把 latency 和 LUT 都搞差了。

所以 O6 我想试一个更规整的方向：把完整 block 和 tail block 分开。完整 block 理论上不需要每次都判断 `i < current_N`、`j < current_M`、`k < current_K`，如果能把这些边界判断从主路径里拿掉，HLS 也许能生成更简单的 mux 和控制逻辑。

## 我改了什么

这次在 `gemm_scheduler.cpp` 里加了一个开关：

```cpp
GZY_ACCEL_FULL_BLOCK_FAST
```

打开以后，scheduler 会先判断当前 block 是否是完整块：

```text
full_nm  = current_N == BLOCK_N && current_M == BLOCK_M
full_nmk = full_nm && current_K == BLOCK_K
```

如果是完整块，就走新加的 full path：

```text
load_ab_block_full()
compute_block_full()
store_c_block_full()
```

如果不是完整块，还是走原来的 boundary path：

```text
load_ab_block()
compute_block()
store_c_block()
```

这版没有改 `gemm_core_mac()`，也没有改 GEMM 数学功能，只是在 scheduler 层多做了一条完整块路径。

## 验证命令

我用单独 Tcl 跑了两个 O6 工程：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log13_o6_fastpath.tcl
```

脚本里包含两个 case：

```text
O6a: TILE=14, BLOCK=112, row_unroll=1, full-block fast path
O6b: TILE=14, BLOCK=112, row_unroll=2, full-block fast path
```

两个 case 都跑了：

```text
csim_design
csynth_design
cosim_design -rtl verilog
```

## C-sim 结果

O6a：

```text
[V1] N=128 K=128 M=128 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=2097152
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=35200361
[V1] PASS
```

O6b：

```text
[V1] N=128 K=128 M=128 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=2097152
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=35200361
[V1] PASS
```

功能上说明 full path 没有破坏结果。

## C-synth 和 C/RTL cosim 结果

| Case | C-sim | C-synth | Verilog cosim | BRAM18K | DSP | FF | LUT | Estimated clock | RTL latency |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O1 对照 | PASS | PASS | PASS | 56 | 196 | 33282 | 49023 | 7.165 ns | 381634 |
| O2 对照 | PASS | PASS | PASS | 84 | 196 | 34317 | 67546 | 7.143 ns | 317122 |
| O6a | PASS | PASS | PASS | 56 | 392 | 62788 | 69099 | 7.218 ns | 381634 |
| O6b | PASS | PASS | PASS | 84 | 392 | 64184 | 134637 | 7.143 ns | 317186 |

对应报告路径：

```text
vitis_hls_project/accel_log13_o6a_tile14_fastpath_ru1_128/solution1/
vitis_hls_project/accel_log13_o6b_tile14_fastpath_ru2_128/solution1/
```

## 这次学到的东西

这版最重要的发现是：**full-block fast path 功能是对的，但这个写法并没有带来性能提升，反而让资源变得更差。**

我原本以为把完整块的边界判断拿掉，HLS 会把主路径变简单。但综合结果显示，HLS 实际上把 `compute_block_full()` 和原来的 `compute_block()` 两套路径都综合进去了。因为代码里仍然保留了 tail block 的 fallback path，所以硬件并不是“替换掉旧路径”，而是“多了一套路径”。

结果就是：

```text
DSP: 196 -> 392
LUT: O6b 到 134637
latency: 基本没有改善
```

对 ZYNQ-7020 来说，这个版本资源明显不可接受。尤其 `DSP=392` 已经超过板上 220 个 DSP48 的数量，说明 O6 这种“同时保留 full path 和 boundary path”的写法不能作为最终方向。

## 更新 roofline 后的结果

我把 O6a/O6b 加进：

```text
python/analysis/roofline_experiments.csv
```

重新运行：

```bash
python3 python/analysis/roofline_model.py
```

输出摘要：

```text
O6a: bound=internal/scheduler-bound, actual=5.495 MAC/cycle, attainable=128.000, latency/roof=23.29x
O6b: bound=internal/scheduler-bound, actual=6.612 MAC/cycle, attainable=128.000, latency/roof=19.36x
```

O6a 和 O1 的 latency 一样；O6b 和 O2 基本一样，甚至略差一点。也就是说，O6 没有缩小 roofline gap。

## 我的结论

O6 这条路跑通了，但不是一个好优化。

我现在对 full-block fast path 的理解变清楚了：这个想法本身可能仍然有价值，但不能用“一个 top 里同时保留 full path 和 boundary path”的方式做。这样 HLS 会为了两条路径都生成硬件，导致资源爆炸。

如果后面还想继续做这个方向，应该换成更干净的实验方式：

```text
1. 单独建一个只支持 full block 的 benchmark top，不保留 boundary fallback。
2. 或者把 full/tail 拆成不同 kernel，先只测 full path 的纯性能上限。
3. tail block 在系统层单独调度，而不是塞进同一个高性能主路径里。
```

所以这一轮我会暂时停止沿 O6 继续加码。当前最好点仍然是 O2：`row_unroll=2` 有明确 latency 收益，但资源效率下降。下一步更应该围绕 O2 做更细的 bank/unroll sweep，或者从数据流层面考虑 double buffering / load-compute overlap，而不是继续复制一套 full path。

