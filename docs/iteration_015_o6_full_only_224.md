# Iteration 015：O6 full-only 编译期开关验证

## 我这一版想解决什么

前面 O6 的 `GZY_ACCEL_FULL_BLOCK_FAST` 是运行时判断：

```cpp
if (full_nmk) {
    load_ab_block_full(...);
    compute_block_full(...);
} else {
    load_ab_block(...);
    compute_block(...);
}
```

这个写法虽然能表达完整 block 走 fast path，但 HLS 会把 full path 和 fallback path 都放进同一个综合结果里。之前 O6 的 DSP 直接翻倍，就是这个问题。

这次我想按“编译期 full-only”的方式重做一次 O6 补充实验。前一次我把 O1 也做成了静态 `224`，这个对照不够干净，因为 HLS 可能已经把普通路径里的边界判断常量传播掉了。现在我把对照改成：

```text
O1_224_generic: N/K/M 从顶层端口输入，综合器不知道一定是 224
O6c_fullonly_224: N/K/M 在编译期固定为 224，只保留 full-only path
```

也就是当我明确知道：

```text
N = K = M = 224
BLOCK_N/K/M = 112
TILE = 14
```

每个大 block 都是完整 block 时，直接让代码在预处理阶段只保留 full-only 版本，普通边界判断、补零和 fallback 路径都不进入这个 HLS 工程。

## 我改了什么

在 `hls/src/gemm_scheduler.cpp` 里新增：

```cpp
GZY_ACCEL_FULL_ONLY
```

默认值是 `0`，不影响前面的 O1/O2/O4/O5/O7 脚本。打开时会走：

```cpp
#if GZY_ACCEL_FULL_ONLY
    load_ab_block_full(...);
    compute_block_full(...);
    store_c_block_full(...);
#else
    原来的 boundary-safe 路径
#endif
```

我还加了编译期检查，要求 bench 维度必须是 block 的整数倍：

```cpp
GZY_ACCEL_BENCH_N % GZY_ACCEL_BLOCK_N == 0
GZY_ACCEL_BENCH_K % GZY_ACCEL_BLOCK_K == 0
GZY_ACCEL_BENCH_M % GZY_ACCEL_BLOCK_M == 0
```

另外 `load_ab_block_full()` 对 `BLOCK_N == BLOCK_M` 做了编译期特化。在当前 `112/112/112` 配置下，full loader 里不再写 `if (x < ACCEL_BLOCK_N)` 或 `if (x < ACCEL_BLOCK_M)` 这种兼容不同 block 形状的判断。

新增脚本：

```text
hls/scripts/run_hls_accel_log16_o6_full_only_224.tcl
```

这个脚本跑两个 case：

```text
accel_log16_o1_tile14_loadab_224_generic
accel_log16_o6_fullonly_tile14_loadab_224
```

二者运行规模都是：

```text
N/K/M = 224
TILE = 14
BLOCK_N/K/M = 112
LOAD_AB_PARALLEL = 1
LOCAL_ROW_UNROLL = 1
```

区别是：

```text
O1_224_generic:    GZY_ACCEL_FULL_ONLY=0, GZY_ACCEL_RUNTIME_NKM=1
O6c_fullonly_224:  GZY_ACCEL_FULL_ONLY=1, GZY_ACCEL_RUNTIME_NKM=0
```

`GZY_ACCEL_RUNTIME_NKM=1` 时，`gemm_scheduler_top()` 会多出 `N/K/M` 三个 `ap_none` 标量输入端口。testbench 仍然传入 224，但这个值对 HLS 综合不是编译期常量。

## 验证过程

运行命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log16_o6_full_only_224.tcl
```

两边 C-sim 都通过：

```text
[V1] N=224 K=224 M=224 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=11239424
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=-159053159
[V1] PASS
```

两边 Verilog cosim 也都通过：

| Case | C-sim | C-synth | Verilog cosim | RTL latency |
| --- | --- | --- | --- | --- |
| O1_224_generic | PASS | PASS | PASS | 381879 cycles |
| O6c full-only 224 | PASS | PASS | PASS | 381634 cycles |

## 结果

综合结果：

| Case | BRAM18K | DSP | FF | LUT | Estimated clock | RTL latency |
| --- | --- | --- | --- | --- | --- | --- |
| O1_224_generic | 56 | 199 | 35271 | 51289 | 7.653 ns | 381879 |
| O6c full-only 224 | 56 | 196 | 29822 | 19539 | 7.263 ns | 381634 |

因为 `O1_224_generic` 的 `N/K/M` 是顶层运行时输入，C-synth summary 里的 top latency 会显示 `?`；这里的 latency 使用 Verilog cosim report 里的实际 224 测试值。

模块实例上，这次 full-only 是成功的。O1_224_generic 的 report 里是：

```text
load_ab_block
compute_block
store_c_block
```

O6c full-only 的 report 里是：

```text
load_ab_block_full
compute_block_full
store_c_block_full
```

也就是说，普通 boundary-safe 模块没有被一起综合进 O6c。这一点达到了本轮目的。

这次对照后，资源结论变清楚了：

```text
O1_224_generic LUT: 51289
O6c_fullonly_224 LUT: 19539
```

这说明当 `N/K/M` 对综合器未知时，generic 边界判断、补零和 partial write 控制确实会带来很大的 LUT/FF 代价。这个更接近以后接 DDR、运行时形状由控制寄存器或指令给出的情况。

但是 latency 只从 `381879` 降到 `381634`，只差 `245 cycles`。所以边界判断对资源影响很大，但不是当前延迟的主要来源。

## 我学到的东西

这次实验说明：把 runtime full/fallback 分支改成 compile-time full-only 是必要的工程写法，因为它确实避免了 O6 之前“双路径一起综合、DSP 翻倍”的问题，也能在 full-only 场景下显著减少 generic 边界控制资源。

但它不是当前 O1 的主要 latency 瓶颈。原因是：

```text
1. O1_224_generic 和 O6c_fullonly_224 的 RTL latency 只差 245 cycles。
2. 两边 load_ab、local load、store 的关键 loop 仍然基本都是 II=1。
3. 总 latency 仍然主要由现有 block/tile 调度结构决定。
4. full-only 对资源更敏感，对当前 latency 不敏感。
```

所以这次 O6 补充版的结论是：

```text
full-only 是正确的代码组织方式；
当 N/K/M 编译期可知且都是 block 整数倍时，它可以明显降低资源；
当后面接 DDR、N/K/M 运行时才知道时，仍然需要 O1_224_generic 这一类 generic 路线；
后续不能再把“消除边界判断”当作主要 latency 突破口。
```

我觉得下一步应该更明确地转向：

```text
load-compute overlap
double buffer
DATAFLOW
减少 A/B/C buffer 到 local tile 的串行搬运时间
```

也就是不再只盯着 boundary 判断，而是让外层 block load、local tile load 和 MAC 之间真正重叠起来。
