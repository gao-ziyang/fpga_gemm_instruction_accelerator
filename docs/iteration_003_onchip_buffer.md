# 迭代日志 003：加入片上 buffer

## 本次迭代目标

在 002 的 tiled GEMM 基础上加入片上缓存：

```text
A -> A_bram
B 的当前列块 -> B_bram
A_bram/B_bram -> localA/localB
localC 完成 4x4 tile 累加
```

本次仍保持：

```text
N = 8
K = 8
M = 8
GEMM_TILE = 4
GEMM_BLOCK_M = 8
```

不处理非整除边界，不做右移量化，目的只是观察 buffer 对调度的影响。

## 为什么这么做

002 的综合警告说明瓶颈在 B 的读端口。FPGA 上 GEMM 的核心不是只写三重循环，而是要安排数据复用：

1. A 的一整块先缓存到 `A_bram`。
2. B 按输出列块缓存到 `B_bram`。
3. 每个 `4x4` 计算 tile 再从 BRAM 搬入完全 partition 的 `localA/localB`。
4. `dot_k` 对 `localA/localB` 做并行读和乘加。

这比直接读外部 A/B 更接近实际 FPGA GEMM 的结构：外部存储负责搬运，BRAM 负责复用，寄存器级 tile 负责并行 MAC。

## 本次改动

| 文件 | 改动 |
| --- | --- |
| `hls/src/gemm_core.cpp` | 增加 `A_bram[GEMM_MAX_N][GEMM_MAX_K]` 和 `B_bram[GEMM_MAX_K][GEMM_BLOCK_M]`。 |
| `hls/src/gemm_core.cpp` | `A_bram/B_bram` 使用 `BIND_STORAGE ram_2p impl=bram`。 |
| `hls/src/gemm_core.cpp` | 增加 `localA/localB/localC`，三者均 `ARRAY_PARTITION complete dim=0`。 |
| `hls/src/gemm_core.cpp` | `dot_k` 只从 `localA/localB` 读取，避免直接读外部 B。 |

## 验证方式

### Python baseline

仍使用 002 的 `8x8x8` case：

```text
[PY] checksum=358080
```

### HLS C simulation

关键输出：

```text
[TB] mismatch_count=0
[TB] max_abs_error=0
[TB] checksum=358080
[TB] PASS
INFO: [SIM 211-1] CSim done with 0 errors.
```

### HLS C synthesis

关键输出：

```text
Pipelining result : Target II = 1, Final II = 1, loop 'load_a_bram'
Pipelining result : Target II = 1, Final II = 1, loop 'load_b_bram'
Pipelining result : Target II = 1, Final II = 1, loop 'load_local_a'
Pipelining result : Target II = 1, Final II = 1, loop 'load_local_b'
Pipelining result : Target II = 1, Final II = 1, loop 'dot_k'
Pipelining result : Target II = 1, Final II = 1, loop 'write_c'
**** Loop Constraint Status: All loop constraints were satisfied.
**** Estimated Fmax: 138.78 MHz
```

综合报告摘要：

| 指标 | 数值 |
| --- | --- |
| Target clock | 10.00 ns |
| Estimated clock | 7.205 ns |
| Estimated Fmax | 约 138.78 MHz |
| BRAM_18K | 2 |
| DSP | 16 |
| FF | 2914 |
| LUT | 2439 |

## 本次结果分析

本次最重要的结果是 `dot_k` 的 `Final II` 从 002 的 `2` 变成了 `1`。这说明片上 buffer 和局部完全 partition 的确解决了局部计算阶段的读端口瓶颈。

代价也很清楚：

1. BRAM 从 0 变成 2，因为引入了 `A_bram/B_bram`。
2. DSP 到 16，符合一个 `4x4` tile 每个 `kk` 周期并行更新 16 个输出元素的结构。
3. Fmax 从约 146 MHz 降到约 139 MHz，原因是并行 MAC 和调度逻辑更复杂。

这个 trade-off 是合理的：为了更高吞吐，使用更多 DSP/BRAM 换取 `II=1` 的计算流水。

