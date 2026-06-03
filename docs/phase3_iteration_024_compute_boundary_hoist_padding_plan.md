# Phase 3 Iteration 024：把边界判断从 compute_block 喂数路径移出去

## 我这一版想解决什么

通过 HLS report、latency 模型和源码对照，我发现当前 `TILE=14/BLOCK=112/MAX=1024` 版本的主要资源压力在：

```text
compute_block
localA/localB/localC feeding
bank select mux
boundary select mux
zero-fill mux
```

尤其是 `compute_block` 里出现了大量 8-bit mux：

```text
mux_164_8_1_1 约 392 个
总计约 25480 LUT
```

另一个可选方向是显式 banked buffer，也就是把 `A_buf/B_buf/C_buf` 的 bank 维度直接写出来，让 HLS 少推断 cyclic partition 的 bank。这个方向仍然有价值，但在动存储布局之前，我先把更明显的重复边界判断问题单独拎出来。

但是继续对照源码后，我发现还有一个更基础的问题：边界判断不应该出现在 `compute_block` 的最高并行喂数路径里。

## 当前问题在哪里

当前 `compute_block()` 里，A/B 从片上 buffer 搬到 local tile 时还在做边界判断：

```cpp
if (ii < GEMM_TILE && bi < current_N && bk < current_K) {
    localA[ii][kk] = A_buf[bi][bk];
} else if (ii < GEMM_TILE) {
    localA[ii][kk] = 0;
}
```

以及：

```cpp
if (kk < GEMM_TILE && bk < current_K && bj < current_M) {
    localB[kk][jj] = B_buf[bk][bj];
} else if (kk < GEMM_TILE) {
    localB[kk][jj] = 0;
}
```

这里的问题不是功能错，而是位置太敏感。

`localA[14][14]` 有 196 个元素，`localB[14][14]` 也有 196 个元素，并且这些循环被 `UNROLL` 展开。这样每个元素附近都可能带着：

```text
bank select
runtime index select
boundary compare
data-or-zero mux
```

这些逻辑乘上 392 个元素，就很容易把 LUT 推爆。

## 为什么可以把判断移出去

因为当前 `load_ab_block()` 本来已经做了 padding。

A 的 load：

```cpp
if (x < current_N && k < current_K) {
    A_buf[x][k] = A_mem[...];
} else {
    A_buf[x][k] = 0;
}
```

B 的 load：

```cpp
if (k < current_K && x < current_M) {
    B_buf[k][x] = B_mem[...];
} else {
    B_buf[k][x] = 0;
}
```

这说明进入 `compute_block()` 之前，当前 block 里非法的 A/B 位置已经是 0。

所以 `compute_block()` 内部理论上可以直接读：

```cpp
localA[ii][kk] = A_buf[ti + ii][tk + kk];
localB[kk][jj] = B_buf[tk + kk][tj + jj];
```

这样做仍然支持任意 `N/K/M` 的尾块，因为边界处理没有消失，只是从：

```text
A_buf/B_buf -> localA/localB
```

移到了：

```text
DDR -> A_buf/B_buf
```

也就是从最高并行度、最容易炸 mux 的 compute feeding 路径，移动到了低并行度的 load 阶段。

## C_buf 也可以类似简化

当前 localC 读取大致是：

```cpp
if (reset_c) {
    localC[ii][jj] = 0;
} else if (bi < current_N && bj < current_M) {
    localC[ii][jj] = C_buf[bi][bj];
} else {
    localC[ii][jj] = 0;
}
```

可以考虑改成：

```cpp
localC[ii][jj] = reset_c ? 0 : C_buf[ti + ii][tj + jj];
```

写回 C_buf 也可以先改成无条件：

```cpp
C_buf[ti + ii][tj + jj] = localC[ii][jj];
```

最后只有 `store_c_block()` 写 DDR 时保留边界：

```cpp
if (i < current_N && j < current_M) {
    C_mem[...] = C_buf[i][j];
}
```

这样是安全的，原因是：

```text
1. 非法 C_buf 元素最终不会写回 DDR；
2. GEMM 的每个 C[i][j] 是独立累加，非法 C 元素不会污染合法 C 元素；
3. reset_c=true 时会把 localC 清零；
4. A/B 的非法输入已经在 load 阶段 padding 为 0；
5. store_c_block 仍然负责阻止越界 DDR 写。
```

也就是说，边界判断仍然存在，只是不再放在 compute 内部最并行的位置。

## 这个优化和显式 bank 的关系

显式 banked buffer 主要针对：

```text
HLS 从 cyclic partition 二维数组推断 bank 时生成的大 mux
```

这一版的 boundary hoist 主要针对：

```text
compute feeding 阶段每个 localA/localB/localC 元素附近的 runtime boundary compare 和 zero-fill mux
```

两者不是冲突关系。

但从改动优先级看，我现在觉得应该先试 boundary hoist：

```text
1. 改动更小；
2. 不改变 A_buf/B_buf/C_buf 的存储布局；
3. 不影响 DDR 布局；
4. 直接命中 compute_block 里的边界判断重复问题；
5. C-sim 更容易验证。
```

如果 boundary hoist 后 `compute_block` 的 LUT 已经明显下降，就可以再决定是否继续做显式 bank。

## 对 latency 和资源的预期

资源方面，我预期收益比较大，尤其是 LUT。

原因是当前最重的地方不是 MAC 本体，而是：

```text
bank select + boundary compare + zero-fill mux
```

把 `current_N/current_K/current_M` 判断从 `localA/localB/localC` 的 unroll feeding 里拿掉，有机会明显减少：

```text
mux_164_8_1_1
HLS Multiplexer
Expression LUT
```

latency 方面要更谨慎。

当前 `compute_block` 的 loop trip count 不会因为这个改动变少，仍然是 full block 计算：

```text
8 x 8 个 output tile
每个 tile 8 个 K tile
```

所以 HLS 报告里的 cycle latency 不一定大幅下降。更可能的收益是：

```text
1. LUT 降低；
2. timing/critical path 改善；
3. HLS scheduler 更容易保持 II=1；
4. Vivado implementation 更容易放进 ZYNQ-7020；
5. 如果 critical path 变短，最终能更稳地跑目标时钟。
```

所以这轮的第一目标是资源，尤其是 LUT；latency 如果下降是额外收益。

## 正确性需要满足的条件

这个优化成立需要几个前提：

```text
1. load 阶段必须完整覆盖 A_buf/B_buf 的所有 block 位置；
2. 对非法 A/B 元素必须写 0；
3. compute 阶段的 tile 循环不能访问 A_buf/B_buf/C_buf 数组范围外；
4. store_c_block 必须继续保留 current_N/current_M 边界判断；
5. BLOCK_N/BLOCK_K/BLOCK_M 最好是 TILE 的整数倍。
```

当前主配置满足：

```text
BLOCK_N = 112
BLOCK_K = 112
BLOCK_M = 112
TILE    = 14
112 / 14 = 8
```

如果后面改成 fallback：

```text
TILE=12
BLOCK=96
```

也同样整除。

但如果尝试 `TILE=13` 这种不能整除 `112` 的配置，就不能简单套用这个 full-block compute 写法，除非同时重做 block/tile 边界。

## 后续路线

下一步建议单独做一个 HLS 版本，不覆盖当前 baseline：

```text
project = accel_axi_o1_1024_boundary_hoist
TILE = 14
BLOCK = 112
MAX = 1024
FULL_ONLY = 0
FULL_BLOCK_FAST = 0
```

只改：

```text
compute_block 内部 localA/localB/localC 的边界判断
```

暂时不改：

```text
A_buf/B_buf/C_buf 显式 bank 布局
TILE
BLOCK
AXI interface
instruction format
```

验证顺序：

```text
1. C-sim：确认 112 test PASS；
2. C-synth：看 top LUT、compute_block LUT；
3. 对比 mux_164_8_1_1 数量；
4. 如果 LUT < 53200，再考虑导出 IP；
5. 如果仍然超，再叠加显式 banked buffer；
6. 如果还不行，再降到 TILE=12/BLOCK=96。
```

这轮优化的判断标准：

```text
最希望看到：compute_block LUT 明显下降，DSP 仍保持 196。
可以接受：latency cycles 基本不变，但 LUT 回到预算内。
不理想：LUT 变化很小，那说明主要 mux 仍来自 bank select，需要继续显式 bank。
```

## 实际实现

我没有直接改掉原 baseline，而是加了一个宏：

```cpp
GZY_ACCEL_COMPUTE_PADDED_INPUTS
```

默认值仍然是 `0`，所以旧脚本不受影响。只有新脚本打开这个宏时，`compute_block()` 才会相信 `load_ab_block()` 已经完成 A/B padding。

新脚本：

```text
hls/scripts/run_hls_accel_axi_1024_boundary_hoist.tcl
```

新 HLS project：

```text
accel_axi_o1_1024_boundary_hoist
```

关键编译宏：

```text
GZY_GEMM_TILE = 14
GZY_ACCEL_BLOCK_N/K/M = 112
GZY_ACCEL_MAX_N/K/M = 1024
GZY_ACCEL_LOAD_AB_PARALLEL = 1
GZY_ACCEL_COMPUTE_PADDED_INPUTS = 1
GZY_ACCEL_FULL_ONLY = 0
GZY_ACCEL_FULL_BLOCK_FAST = 0
```

为了避免以后误用，我还加了编译期检查：

```cpp
#if GZY_ACCEL_COMPUTE_PADDED_INPUTS
#if (GZY_ACCEL_BLOCK_N % GZY_GEMM_TILE) != 0
#error "GZY_ACCEL_COMPUTE_PADDED_INPUTS requires BLOCK_N to be a multiple of TILE"
#endif
...
#endif
```

也就是说，这个优化要求 block 尺寸能被 tile 整除。当前 `112/14=8`，所以满足。

## 具体改了哪些地方

### 1. localC load

原来：

```cpp
if (reset_c) {
    localC[ii][jj] = 0;
} else if (bi < current_N && bj < current_M) {
    localC[ii][jj] = C_buf[bi][bj];
} else {
    localC[ii][jj] = 0;
}
```

新版本：

```cpp
localC[ii][jj] = reset_c ? (gemm_acc_t)0 : C_buf[bi][bj];
```

### 2. localA load

原来：

```cpp
if (bi < current_N && bk < current_K) {
    localA[ii][kk] = A_buf[bi][bk];
} else {
    localA[ii][kk] = 0;
}
```

新版本：

```cpp
localA[ii][kk] = A_buf[bi][bk];
```

### 3. localB load

原来：

```cpp
if (bk < current_K && bj < current_M) {
    localB[kk][jj] = B_buf[bk][bj];
} else {
    localB[kk][jj] = 0;
}
```

新版本：

```cpp
localB[kk][jj] = B_buf[bk][bj];
```

### 4. localC store back

原来：

```cpp
if (bi < current_N && bj < current_M) {
    C_buf[bi][bj] = localC[ii][jj];
}
```

新版本：

```cpp
C_buf[bi][bj] = localC[ii][jj];
```

外层 `store_c_block()` 仍然保留：

```cpp
if (i < current_N && j < current_M) {
    C_mem[...] = C_buf[i][j];
}
```

所以非法 C 元素即使在片上 `C_buf` 里被计算，也不会写回 DDR。

## 验证方法

先用普通 C++ testbench 做了一次轻量功能验证，避免明显改坏：

```text
g++ ... -DGZY_ACCEL_COMPUTE_PADDED_INPUTS=1 ...
/tmp/accel_axi_boundary_hoist_tb
```

结果：

```text
[AXI] N=112 K=112 M=112 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=1404928
[AXI] status=1 expected_status=1
[AXI] mismatch_count=0
[AXI] max_abs_error=0
[AXI] checksum=20974123
[AXI] PASS
```

然后跑 Vitis HLS：

```powershell
& 'C:\Xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat' -f 'C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_axi_1024_boundary_hoist.tcl'
```

HLS C-sim 结果：

```text
[AXI] mismatch_count=0
[AXI] max_abs_error=0
[AXI] PASS
INFO: [SIM 1] CSim done with 0 errors.
```

`solution1.log` 里有一次：

```text
ERROR: [IMPL 213-28] Failed to generate IP.
```

这是 Vivado/Vitis HLS 2020.2 的 `core_revision` 老问题。脚本后面的 workaround 已经把 `Revision` 改成 `1` 并重新打包，最终已经生成：

```text
solution1/impl/ip/component.xml
solution1/impl/ip/xilinx_com_hls_accelerator_top_axi_1_0.zip
```

所以这次 C-synth 和 IP 文件都可用。

## 资源对比

### 顶层资源

| 版本 | BRAM18K | DSP | FF | LUT |
| --- | ---: | ---: | ---: | ---: |
| 原 `accel_axi_o1_1024` | 60 | 199 | 36,892 | 53,581 |
| `boundary_hoist` | 60 | 199 | 33,302 | 23,708 |
| 变化 | 0 | 0 | -3,590 | -29,873 |

这个结果比预期更明显。LUT 从超过 ZYNQ-7020 上限：

```text
53581 / 53200 ~= 100%
```

降到：

```text
23708 / 53200 ~= 44%
```

也就是说，`TILE=14` 的 196-DSP MAC 阵列保住了，同时 LUT 已经不再卡住。

### execute_instruction_stream / gemm_scheduler

| 模块 | 原 LUT | 新 LUT | LUT 变化 | 原 FF | 新 FF | FF 变化 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `execute_instruction_stream` | 52,077 | 22,204 | -29,873 | 35,649 | 32,059 | -3,590 |
| `gemm_scheduler` | 51,632 | 21,759 | -29,873 | 35,351 | 31,761 | -3,590 |

这说明资源下降几乎全部发生在 scheduler/compute 这条路径里，AXI-Lite 和 AXI master 没有变化。

### compute_block

| 模块 | 原 LUT | 新 LUT | LUT 变化 | 原 FF | 新 FF | FF 变化 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `compute_block` | 46,608 | 16,735 | -29,873 | 32,064 | 28,474 | -3,590 |
| `gemm_core_mac` | 5,210 | 5,210 | 0 | 6,348 | 6,348 | 0 |

这个结果非常关键：

```text
MAC 阵列本体完全没变；
减少的 LUT/FF 全部来自 compute_block 外围 feeding/control。
```

这正好验证了前面的判断：问题不是 DSP MAC 本体，而是 `localA/localB/localC` 喂数路径上的边界判断、补零和 mux。

## compute_block 内部小项对比

| 小项 | 原资源 | 新资源 | 变化 |
| --- | ---: | ---: | ---: |
| Expression LUT | 1,254 | 648 | -606 |
| Instance LUT | 32,188 | 6,708 | -25,480 |
| HLS Multiplexer LUT | 13,166 | 9,379 | -3,787 |
| Register FF | 25,716 | 22,126 | -3,590 |
| Total LUT | 46,608 | 16,735 | -29,873 |

最关键的一项是 `Instance LUT`：

```text
32188 -> 6708
减少 25480 LUT
```

这正好对应原来那些大 mux：

```text
mux_164_8_1_1:
  原来约 392 个 instance
  每个 65 LUT
  392 x 65 = 25480 LUT

boundary_hoist 后：
  mux_164_8_1_1 消失
```

`mux_144_32_1_1` 仍然存在：

```text
14 个 instance
每个 57 LUT
总计 798 LUT
```

这说明本轮主要消掉的是 A/B 的 8-bit local feeding 大 mux；C 相关的一些 32-bit mux 和其他控制 mux 仍然存在。

`HLS Multiplexer` 也下降：

```text
13166 -> 9379
减少 3787 LUT
```

这部分大概率来自 `current_N/current_K/current_M` 比较、条件赋值、data-or-zero 选择减少后，FSM 和条件路径也随之简化。

## latency 和 timing 对比

cycle latency 基本不变：

| 模块 | 原 latency | 新 latency | 变化 |
| --- | ---: | ---: | ---: |
| `load_ab_block` | 25,101 | 25,101 | 0 |
| `compute_block` | 28,865 | 28,865 | 0 |
| `store_c_block` | 12,562 | 12,562 | 0 |

`compute_block` 内部 loop 也不变：

```text
load_local_c_group   II=1
load_local_a_group   II=1
load_local_b_group   II=1
store_local_c_group  II=1
gemm_core_mac        II=1
```

这符合预期，因为这次优化没有减少 tile 数，也没有减少 K tile 数，只是把每个 local element 周围的判断和 mux 去掉。

但是 timing 有改善：

| 位置 | 原 estimated clock | 新 estimated clock |
| --- | ---: | ---: |
| top | 7.653 ns | 7.300 ns |
| `compute_block` | 7.653 ns | 6.779 ns |
| estimated Fmax | 约 130.67 MHz | 约 136.99 MHz |

所以这轮主要收益是：

```text
1. LUT 大幅下降；
2. FF 小幅下降；
3. compute critical path 变短；
4. latency cycle 数不变；
5. DSP/BRAM 不变。
```

## 这轮结论

这次实验基本证明：之前 `compute_block` 里的边界判断确实放在了太并行、太敏感的位置。

原来每个 `localA/localB` 元素都带着：

```text
runtime boundary compare
zero-fill select
bank/data select mux
```

所以形成了约 392 个 `mux_164_8_1_1`，直接吃掉约 `25480 LUT`。

把边界处理前移到 `load_ab_block()`，并把最终 DDR 写回边界保留在 `store_c_block()` 后，`compute_block()` 可以保持 full-block datapath：

```text
localA = A_buf
localB = B_buf
localC = reset_c ? 0 : C_buf
C_buf  = localC
```

这并没有改变数学结果，因为非法 A/B 已经 padding 为 0，非法 C 最终不会写回 DDR。

这一版是目前非常成功的一刀：

```text
TILE=14 保住；
DSP=199 保住；
BRAM=60 不变；
LUT 从 53581 降到 23708；
compute latency cycles 不变。
```

因此，下一步不需要立刻降到 `TILE=12`。更合理的是：

```text
1. 先把 boundary_hoist 作为新的 1024 主线；
2. 用这个 IP 重新进入 Vivado block design；
3. 上板验证 1008 full-block 和 1024 boundary-block；
4. 如果后续还想压 LUT 或改善 timing，再考虑显式 banked buffer；
5. double buffering / DATAFLOW 暂时继续放后面。
```
