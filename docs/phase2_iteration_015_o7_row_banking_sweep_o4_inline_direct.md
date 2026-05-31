# Phase 2 / Iteration 015：O7 row banking sweep，以及 O4 inline/direct 对照

## 我这一版想解决什么

前面 O6/O7 跑完以后，我对结论做了一次修正。

O6 full-block fast path 不是完全没意义。现在 `N=128,K=128,M=128` 的测试规模还比较小，而且 O6 的写法是在同一个 top 里同时保留 full path 和 boundary fallback，所以 HLS 复制了硬件。这个结果只能说明“当前写法不适合落地”，不能说明 full-block 思路本身没有用。后面如果再做，应该单独做 full-block benchmark，或者在更大 shape 上测，不要在同一个高性能主路径里塞 fallback。

O7 的结论更明确：列方向 banking 是必要基础，行方向 banking 是高代价探索。当前实验结果已经说明行 banking 不适合作为可落地路线，后面没有必要继续加码。

另外我重新检查 O4/O5，发现 `load_local_ab_tile()` 里用了：

```cpp
#pragma HLS INLINE off
```

这个确实可能影响调度，所以这一版补两个对照：

```text
O4inline：保留 helper，但强制 INLINE。
O4_2：不要 helper，直接把 combined local A/B load 写在 compute_block() 里。
```

## 我改了什么

在 `hls/src/gemm_scheduler.cpp` 里新增了几个开关：

```cpp
GZY_ACCEL_LOCAL_AB_HELPER_INLINE
GZY_ACCEL_LOCAL_AB_HELPER_PARTITION
GZY_ACCEL_LOCAL_AB_DIRECT
```

含义是：

```text
GZY_ACCEL_LOCAL_AB_HELPER_INLINE=1
  load_local_ab_tile() 用 #pragma HLS INLINE

GZY_ACCEL_LOCAL_AB_HELPER_PARTITION=0/1
  控制 helper 形参上的 A_buf/B_buf ARRAY_PARTITION

GZY_ACCEL_LOCAL_AB_DIRECT=1
  不调用 helper，直接在 compute_block() 里写 combined local A/B load 循环
```

新增脚本：

```text
hls/scripts/run_hls_accel_log15_o4_inline_direct.tcl
```

这个脚本固定：

```text
N/K/M = 128
TILE = 14
BLOCK_N/K/M = 112
LOAD_AB_PARALLEL = 1
LOCAL_ROW_UNROLL = 1
FULL_BLOCK_FAST = 0
```

然后跑两个 case：

```text
accel_log15_o4_inline_helper_128
accel_log15_o4_2_direct_localab_128
```

我也把旧的 O4/O5 Tcl 补成显式宏：

```text
O4: HELPER_INLINE=0, HELPER_PARTITION=0
O5: HELPER_INLINE=0, HELPER_PARTITION=1
```

这样以后复现实验时不会被默认宏影响。

## O7 结果怎么收敛

这轮已有 O7a/O7b 报告：

| Case | row_unroll | C/RTL cosim | BRAM18K | DSP | FF | LUT | RTL latency |
| --- | --- | --- | --- | --- | --- | --- | --- |
| O2 | 2 | PASS | 84 | 196 | 34317 | 67546 | 317122 |
| O7a | 4 | PASS | 224 | 196 | 42468 | 82719 | 330946 |
| O7b | 7 | PASS | 392 | 196 | 267912 | 193181 | 413890 |

O7c 的工程只看到 C-sim 和综合中间文件，没有完整最终报告，所以不作为有效点。

这个结果说明：

```text
row_unroll=2 已经是探索上限附近。
row_unroll=4 开始 BRAM/LUT 明显爆炸，latency 还比 O2 差。
row_unroll=7 资源完全不可接受，latency 也退化。
```

所以后面我会把“列方向 partition/banking”当成必要结构，但不再继续把行方向 banking 当成落地优化路线。

## O4inline / O4_2 验证结果

运行命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log15_o4_inline_direct.tcl
```

### O4inline

功能结果：

```text
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=35200361
[V1] PASS
```

C-synth / cosim 结果：

| Case | C-sim | C-synth | Verilog cosim | BRAM18K | DSP | FF | LUT | Estimated clock | Latency |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O4inline | PASS | PASS | PASS | 56 | 196 | 43062 | 27623 | 7.165 ns | 2988226 |

这里最重要的不是资源，而是 latency。`O4inline` 的 RTL latency 到了 `2988226`，和之前 O3 的 C-synth 级别很接近，比 O4/O5 的 `721602` 还差很多。

### O4_2 direct

功能和 C-synth：

```text
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=35200361
[V1] PASS
```

| Case | C-sim | C-synth | Verilog cosim | BRAM18K | DSP | FF | LUT | Estimated clock | C-synth latency |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O4_2 direct | PASS | PASS | 未跑完，手动停止 | 56 | 196 | 43062 | 27623 | 7.165 ns | 2979010 |

我在 C-synth 结果已经和 O4inline 完全一致后，停止了第二个长 cosim。因为它已经能说明 direct 写法不会救这条路线，继续跑完整 RTL 只是在重复等几百万周期仿真。

## 这一版我学到的东西

我一开始怀疑 `INLINE off` 可能是 O4/O5 退化的主因。现在看下来，它不是唯一主因。

更准确地说：

```text
helper + INLINE off:
  latency 比 O1/O2 差，但还能在 721602 cycles；
  LUT 很高，说明 mux/端口/控制逻辑很重。

helper + INLINE:
  LUT 降了，但调度退化成接近 2979010 cycles。

direct 写进 compute_block():
  和 helper inline 基本一样，说明问题不是 helper 函数名本身。
```

HLS 在这两版里都提示 combined local A/B load 所在循环受 `gemm_core_mac` 子函数调度影响。我的理解是，把 local A/B load 合并成同一个 pipeline 区域以后，HLS 没有得到更好的并行 load，反而把 load 和 compute 的调度关系变复杂了。

所以 O4/O5 这条线可以收敛为：

```text
合并 local A/B helper 不是可落地方向。
INLINE off/on 都不能把它变成好路线。
后面不要继续在这个方向上消耗时间。
```

## 下一步

我现在更认可这几个方向：

```text
1. 保留列方向 banking/partition，这是喂 TILE x TILE MAC 的基础。
2. 行方向 banking 不继续做落地路线，只作为实验反例记录。
3. O6 full-block 想法先不灰心，但下次要做“只含 full path”的独立 benchmark，或者换更大 shape。
4. 真正值得继续的是 double buffer / DATAFLOW / load-compute overlap，而不是继续折腾 local A/B helper。
```

可以这样跟老师说：

```text
我补做了 O4 helper inline 和 no-helper direct 两个对照。结果说明 O4/O5 的问题不只是 INLINE off，而是 combined local A/B load 这个调度结构本身不适合当前 HLS 生成方式。O7 的 row banking sweep 也说明行方向 banking 资源代价太高，所以后续我会保留列方向 banking，停止行 banking 和 local A/B helper 这两条不可落地路线，把精力转向更清楚的数据流和 double buffering。
```
