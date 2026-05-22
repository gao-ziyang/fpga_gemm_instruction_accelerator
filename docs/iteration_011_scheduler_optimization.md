# Iteration 011：64-bit 指令、TILE=14 和 scheduler 调度优化

## 我这一版想解决什么

这次我开始把前一版 V1/V2/V3 往老师说的方向再推进一点：不要只停留在“功能跑通”，而是开始动调度路径，观察计算阵列、片上缓存和供数之间的关系。

我这一版先做了几件事：

```text
1. 把指令字从 128 bit 改成 64 bit。
2. 把 accelerator 路线的默认测试参数改成 TILE=14、BLOCK_N/K/M=112。
3. 把 scheduler 优化做成可配置宏，方便复现实验阶段。
4. 尝试 A/B block 并行加载。
5. 尝试 local buffer 行方向 banking + row unroll=2。
```

我现在更清楚了：`gemm_core_mac()` 本身已经是比较“并行”的 2D MAC 阵列；真正拖慢整体性能的地方，很可能是 `gemm_scheduler()` 里怎么把数据搬到 `A_buf/B_buf/C_buf`，再怎么从 BRAM 喂给 `localA/localB/localC`。

## 阶段 0：指令改成 64 bit

之前的指令是 128 bit，字段比较宽。这次我改成了 64 bit：

```text
[7:0]     opcode
[19:8]    N - 1
[31:20]   K - 1
[43:32]   M - 1
[49:44]   a_base / 4096
[55:50]   b_base / 4096
[61:56]   c_base / 4096
[63:62]   reserved
```

这里我把 `N/K/M` 存成 `value - 1`，这样 12 bit 字段可以表示 1 到 4096。当前测试的 `1024` 可以正常放进去。

base address 这里我先做了一个简化：因为当前 HLS 单元验证里 `A_mem/B_mem/C_mem` 还是分开的三个数组，所以 base 主要是为了保留“以后接 DDR 指令”的形式感；现在字段按 4096 element 对齐。后面如果真的做统一 DDR 地址空间，这里可能要扩展成多条配置指令，或者把 base 放进寄存器而不是一条 64-bit GEMM 指令里。

我这一版学到的是：64-bit 指令不一定能把所有信息都塞得很舒服，所以指令集不是越“像 CPU”越好，而是要结合具体 accelerator 的寄存器配置和内存模型来设计。

## 阶段 1：TILE=14，BLOCK=112

这一步先不改调度，只把参数从：

```text
TILE = 12
BLOCK = 96
```

换成：

```text
TILE = 14
BLOCK_N/K/M = 112
```

这样 `gemm_core_mac()` 理论上会使用约：

```text
14 x 14 = 196 MAC
```

也就是更接近把 ZYNQ-7020 的 DSP 用满。这个阶段的意义不是保证 latency 一定下降，而是观察当计算阵列变大后，scheduler 能不能喂得上。

## 阶段 2：A/B block 并行加载

原来的调度是：

```text
load_a_block()
load_b_block()
compute_block()
```

这里 `A_mem` 和 `B_mem` 是两个不同数组，`A_buf` 和 `B_buf` 也是两个不同片上 buffer，所以 A 和 B 的 block 加载本身没有数据依赖。

我新增了：

```cpp
load_ab_block()
```

让它在同一个 pipeline 循环里同时做：

```text
A_mem -> A_buf
B_mem -> B_buf
```

我理解这属于“部分并行”：它还没有把 load 和 compute 重叠，只是把 A block 和 B block 的两段串行加载合并成同一段加载。理论上，如果 A/B 两边接口和 BRAM 写口都能支持，它可以把 block 加载时间从接近：

```text
load A + load B
```

压到接近：

```text
max(load A, load B)
```

## 阶段 3：local row banking + row unroll=2

在 `compute_block()` 里面，每个 K tile 都会做：

```text
load_local_a
load_local_b
gemm_core_mac
```

之前虽然 `localA/localB/localC` 是 complete partition，但 `A_buf/B_buf/C_buf` 仍然主要按列方向 partition。这样 local load 还是需要按行一行一行搬：

```text
TILE=14 时，大概每次 localA 要 14 拍，localB 要 14 拍
```

这次我加了一个比较保守的 banking：

```cpp
#pragma HLS ARRAY_PARTITION variable=A_buf cyclic factor=2 dim=1
#pragma HLS ARRAY_PARTITION variable=B_buf cyclic factor=2 dim=1
#pragma HLS ARRAY_PARTITION variable=C_buf cyclic factor=2 dim=1
```

同时把 local load/store 的行方向改成 `row_unroll=2`。也就是说，我不是一下子全展开 14 行，而是先一次搬 2 行。这样资源压力比“全并行 local load”低很多，但能观察 banking 是否对 local load 有帮助。

我没有直接做 `row_unroll=14`，因为这会要求 BRAM bank 和读端口一下子爆炸，可能把 BRAM、mux 和布线压力拉得很高。对 ZYNQ-7020 来说，先做 `2` 是更稳一点的实验。

## 新增的可配置宏

为了让每一版可以复现，我没有把优化写死，而是加了宏：

```text
GZY_ACCEL_LOAD_AB_PARALLEL
  0: load_a_block + load_b_block 串行
  1: load_ab_block 合并加载

GZY_ACCEL_LOCAL_ROW_UNROLL
  1: 原来的 local row 加载方式
  2: 行方向多开一组 bank，并一次搬 2 行
```

这样同一份源码可以跑：

```text
O0: TILE=14, BLOCK=112, serial load
O1: TILE=14, BLOCK=112, A/B block 并行加载
O2: TILE=14, BLOCK=112, A/B block 并行加载 + row banking=2
```

## 当前我能完成的验证

当前 Codex 执行 shell 不能直接启动 Windows 版 Vitis HLS。报错是：

```text
cannot execute binary file
Could not find 64-bit executable ... lnx64.o/vitis_hls does not exist
```

原因不是路径写错，而是这个 Codex shell 没有启用 Windows PE 互操作；它能看到 C 盘文件，但不能执行 Windows `.bat/.exe`。所以这一版我先做了普通 C++ 编译和运行，验证功能逻辑没有错；HLS 的 C-sim/C-synth/C/RTL cosim 需要在 Windows PowerShell 或 Vitis HLS Command Prompt 中运行 Tcl。

我后来又单独尝试了：

```bash
/mnt/c/Windows/System32/cmd.exe /c ver
```

同样报：

```text
cannot execute binary file: Exec format error
```

再直接把 Windows PowerShell 当成 shell 启动，仍然是：

```text
Exec format error
```

检查 `/proc/sys/fs/binfmt_misc` 后，发现当前环境没有注册 Windows PE 执行器；同时 Linux 侧 Vitis HLS wrapper 也找不到 `lnx64.o/vitis_hls`。所以这个失败不是 Tcl 路径写错，也不是你的 Vitis 装错，而是当前 Codex 执行环境无法直接调用 Windows 工具链。

普通 C++ 验证命令使用了 Xilinx HLS 的 `ap_int.h`：

```bash
g++ -std=c++11 -Wno-unknown-pragmas \
  -Ihls/src -Ihls/tb \
  -I/mnt/c/xilinx/Vitis_HLS/2020.2/include \
  -DGZY_GEMM_TILE=14 \
  -DGZY_GEMM_BLOCK_M=14 \
  -DGZY_ACCEL_BLOCK_N=112 \
  -DGZY_ACCEL_BLOCK_K=112 \
  -DGZY_ACCEL_BLOCK_M=112 \
  -DGZY_ACCEL_LOAD_AB_PARALLEL=1 \
  -DGZY_ACCEL_LOCAL_ROW_UNROLL=2 \
  -DGZY_ACCEL_MAX_N=128 \
  -DGZY_ACCEL_MAX_K=128 \
  -DGZY_ACCEL_MAX_M=128 \
  -DGZY_ACCEL_BENCH_N=128 \
  -DGZY_ACCEL_BENCH_K=128 \
  -DGZY_ACCEL_BENCH_M=128 \
  hls/src/gemm_core.cpp \
  hls/src/gemm_scheduler.cpp \
  hls/src/accelerator_instruction.cpp \
  hls/src/accelerator_top.cpp \
  hls/tb/tb_accelerator_top.cpp
```

输出结果：

```text
[V1] N=128 K=128 M=128 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=2097152
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=35200361
[V1] PASS

[V1] N=128 K=128 M=128 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=2097152
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=35200361
[V1] PASS

[V1] N=128 K=128 M=128 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=2097152
[V1] status=0 expected_status=0
[V1] mismatch_count=0
[V1] max_abs_error=0
[V1] checksum=35200361
[V1] PASS

[V2] N=128 K=128 M=128 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=2097152
[V2] status=1 expected_status=1
[V2] mismatch_count=0
[V2] max_abs_error=0
[V2] checksum=35200361
[V2] PASS

[V3] N=128 K=128 M=128 TILE=14 BLOCK_N=112 BLOCK_K=112 BLOCK_M=112 total_mac=2097152
[V3] status=1 expected_status=1
[V3] mismatch_count=0
[V3] max_abs_error=0
[V3] checksum=35200361
[V3] PASS
```

上面三个 V1 分别对应：

```text
O0: serial load
O1: A/B block 并行加载
O2: A/B block 并行加载 + row banking=2
```

我还额外做了 `1024` 默认规模的 C++ 编译检查，确认当前 `TILE=14/BLOCK=112/64-bit instruction` 组合在普通 C++ 编译层面没有语法或链接问题；但没有在 CPU 上直接运行 `1024^3`，因为那会变成 10 亿级循环，和 HLS 验证目的不匹配。

## Windows Vitis HLS 需要跑的命令

这一版新增了完整 Tcl：

```text
hls/scripts/run_hls_accel_log11_opt.tcl
```

在 Windows PowerShell 或 Vitis HLS Command Prompt 中运行：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log11_opt.tcl
```

这个脚本会依次跑：

| Case | 规模 | 目的 |
| --- | --- | --- |
| `accel_log11_o0_tile14_serial_128` | 128 | TILE=14 串行调度基线 |
| `accel_log11_o1_tile14_loadab_128` | 128 | A/B block 并行加载 |
| `accel_log11_o2_tile14_loadab_bank2_128` | 128 | A/B 并行加载 + row banking=2 |
| `accel_log11_v2_decode64_bank2_128` | 128 | 64-bit 指令 decode 路径 |
| `accel_log11_v3_top64_bank2_128` | 128 | 64-bit accelerator_top 路径 |
| `accel_log11_v1_tile14_bank2_1024` | 1024 | 最终大规模 scheduler C-sim/C-synth |
| `accel_log11_v3_top64_bank2_1024` | 1024 | 最终大规模 accelerator_top C-sim/C-synth |

其中 128 规模会跑：

```text
C-sim + C-synth + C/RTL cosim
```

1024 规模只跑：

```text
C-sim + C-synth
```

因为 `1024^3` 的 RTL cosim 会非常慢，不适合作为每轮迭代的必跑项。

## 这一版我对“是否每一步都跑 V1/V2/V3”的理解

我现在觉得没必要每一个 scheduler 小优化都完整跑 V1/V2/V3 三套大规模验证。更合理的是：

```text
改 scheduler 本体：
  先跑 V1，因为 V1 直接测 gemm_scheduler。

改 instruction/decode：
  跑 V2。

改 accelerator_top 接口：
  跑 V3。

最终阶段：
  V1/V2/V3 都跑一遍小规模 RTL cosim；
  V1/V3 再跑一遍 1024 C-sim/C-synth。
```

这也是这次 Tcl 的组织方式。这样既能覆盖功能，又不会每次都做非常耗时的重复验证。

## 这一版还没有完成的 HLS 报告

因为当前 shell 不能执行 Windows Vitis HLS，所以这一版的 HLS 表格还需要等 Windows 端跑完脚本后补上：

| Case | C-sim | C-synth | C/RTL cosim | BRAM_18K | DSP | FF | LUT | Latency |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O0 serial 128 | 待跑 | 待跑 | 待跑 | 待补 | 待补 | 待补 | 待补 | 待补 |
| O1 loadAB 128 | 待跑 | 待跑 | 待跑 | 待补 | 待补 | 待补 | 待补 | 待补 |
| O2 loadAB+bank2 128 | 待跑 | 待跑 | 待跑 | 待补 | 待补 | 待补 | 待补 | 待补 |
| V2 decode64 128 | 待跑 | 待跑 | 待跑 | 待补 | 待补 | 待补 | 待补 | 待补 |
| V3 top64 128 | 待跑 | 待跑 | 待跑 | 待补 | 待补 | 待补 | 待补 | 待补 |
| V1 final 1024 | 待跑 | 待跑 | 不跑长 cosim | 待补 | 待补 | 待补 | 待补 | 待补 |
| V3 final 1024 | 待跑 | 待跑 | 不跑长 cosim | 待补 | 待补 | 待补 | 待补 | 待补 |

## 后续想法

如果 `loadAB` 和 `bank2` 在 HLS 报告里有明显收益，下一步可以继续试：

```text
1. row_unroll = 4
2. 更激进的 A_buf/B_buf banking
3. double buffer，把当前 compute 和下一块 load 重叠
4. DATAFLOW，把 load/compute/store 拆成更清晰的数据流阶段
```

但我现在也更谨慎了：全并行并不是“把 pragma 全部加满”。如果 bank 数量不够，UNROLL 只会生成巨大的 mux 和冲突；如果 bank 数量太多，又可能把 BRAM 和布线压力拉爆。所以后续每一步都应该看资源和 latency，而不是只看 DSP 数量。
