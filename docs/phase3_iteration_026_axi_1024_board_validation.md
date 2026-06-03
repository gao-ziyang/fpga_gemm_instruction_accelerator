# Phase 3 Iteration 026：1024-capable AXI GEMM IP 上板验证

## 我这一版想解决什么

前面 log24/log25 主要是在 HLS 里解决一个问题：

```text
TILE=14 / BLOCK=112 / MAX=1024 时，generic boundary 版本 LUT 太高，
需要先把 1024-capable IP 做到资源能放进 ZYNQ-7020。
```

log24 通过 boundary hoist 把最大一批边界选择 mux 消掉；log25 继续用 explicit banks 把片上 A/B/C buffer 的 bank 访问写得更明确，让 top LUT 再降一点。到这一版，我想验证的是：

```text
这个 1024-capable 的 HLS IP 不只是 C-sim/C-synth 通过，
而是真的能完成 Vivado implementation、导出 XSA，并在板子上跑 1008/1024 GEMM。
```

这里我没有继续用旧的 `112` bitstream，而是重新走了一遍：

```text
HLS 1024 explicit-banks IP
  -> Vivado block design
  -> bitstream / XSA
  -> Vitis platform/application
  -> PS-PL-DDR board validation
```

## 为什么先测 1008，再测 1024

当前 block size 是：

```text
BLOCK_N = 112
BLOCK_K = 112
BLOCK_M = 112
```

所以：

```text
1008 = 9 x 112
```

`1008x1008x1008` 是完整 block 路径，主要验证大矩阵 full-block 主路径：

```text
所有 block 都是 112 x 112 x 112
没有最后一块 partial boundary
```

而：

```text
1024 = 9 x 112 + 16
```

`1024x1024x1024` 会触发最后的边界 block，主要验证：

```text
current_N/current_K/current_M 边界路径
load 阶段补 0
compute 阶段处理 padded 数据
store 阶段只写合法 C 元素
```

所以这两个测试连起来看，比只跑一个 `1024` 更清楚：

```text
1008 过了，说明大矩阵 full-block 主路径没问题；
1024 过了，说明非整块 boundary 路径也没问题。
```

## HLS IP 选择

这一版上板使用的是 log25 之后资源更松的版本：

```text
hls/scripts/run_hls_accel_axi_1024_explicit_banks.tcl
```

关键宏是：

```text
GZY_GEMM_TILE                  = 14
GZY_ACCEL_BLOCK_N              = 112
GZY_ACCEL_BLOCK_K              = 112
GZY_ACCEL_BLOCK_M              = 112
GZY_ACCEL_MAX_N                = 1024
GZY_ACCEL_MAX_K                = 1024
GZY_ACCEL_MAX_M                = 1024
GZY_ACCEL_BENCH_N/K/M          = 112
GZY_ACCEL_COMPUTE_PADDED_INPUTS = 1
GZY_ACCEL_EXPLICIT_BANKS        = 1
GZY_ACCEL_FULL_ONLY             = 0
GZY_ACCEL_FULL_BLOCK_FAST       = 0
```

这里 `BENCH=112` 是为了让 C-sim 不跑非常慢的 `1024^3` 软件仿真；真正导出的硬件仍然是：

```text
MAX_N/K/M = 1024
m_axi depth = 1024 x 1024 级别
```

HLS C-synth 里这个版本的 top 资源是：

| 资源 | HLS estimate |
| --- | ---: |
| BRAM18K | 60 |
| DSP | 200 |
| FF | 33205 |
| LUT | 22887 |

这比最开始 generic 1024 的 `53581 LUT` 安全很多，适合作为上板候选。

## Vivado 自动化脚本

为了不再手动在 Vivado GUI 里重复配 block design，我加了一个 Vivado Tcl：

```text
scripts/vivado/build_accel_axi_1024_explicit_banks.tcl
```

它做的事情是：

```text
1. 新建 accel_axi_1024_explicit_banks Vivado project；
2. 设置 HLS 1024 explicit-banks IP repo；
3. 刷新 IP catalog；
4. 复用 112 工程已经验证过的 design_1 block design 结构；
5. 连接 PS GP0 -> AXI-Lite control；
6. 连接 HLS m_axi_gmem -> AXI interconnect -> PS HP0；
7. 保持 AXI-Lite base address = 0x40000000；
8. 生成 wrapper；
9. 跑 synthesis / implementation / bitstream；
10. 导出包含 bitstream 的 XSA。
```

运行命令是：

```bat
C:\Xilinx\Vivado\2020.2\bin\vivado.bat -mode batch -source C:\Transformer\gzy_gemm_accel\scripts\vivado\build_accel_axi_1024_explicit_banks.tcl
```

导出的 XSA 是：

```text
C:/Transformer/gzy_gemm_accel/vivado_board/accel_axi_1024_explicit_banks/export/accel_axi_1024_explicit_banks.xsa
```

## Vivado implementation 结果

Vivado 最终成功导出 XSA：

```text
INFO: exported XSA:
C:/Transformer/gzy_gemm_accel/vivado_board/accel_axi_1024_explicit_banks/export/accel_axi_1024_explicit_banks.xsa
```

实现后的资源和时序是：

| 项目 | 结果 |
| --- | ---: |
| Slice LUTs | 22862 / 53200 = 42.97% |
| Slice Registers | 33411 / 106400 = 31.40% |
| Block RAM Tile | 30 / 140 = 21.43% |
| RAMB36E1 | 16 |
| RAMB18E1 | 28 |
| DSPs | 206 / 220 = 93.64% |
| Setup WNS | 3.217 ns |
| Hold WHS | 0.023 ns |

这里 DSP 已经很高：

```text
206 / 220 = 93.64%
```

但 LUT 只有 42.97%，时序也满足。这个结果说明 log24/log25 的资源优化确实起到了关键作用：如果还停留在 generic 1024 的 `53581 LUT` 版本，Vivado 这一步风险会高很多。

实现过程里有一些 Vivado DRC warning，例如 `REQP-1839` 和 DSP pipeline 建议。它们没有变成 critical warning 或 error，而且旧的 112 工程里也见过类似 AXI interconnect / HLS AXI master RAMB 控制相关 warning。当前这一版的关键判断是：

```text
0 Critical Warnings
0 Errors
Timing MET
Bitstream generated
XSA exported
```

## Vitis application

这次 PS 端使用的是：

```text
ps_apps/accel_axi_1024_gemm_test/helloworld.c
```

它和 112 版最大的区别是：

```text
1. 使用更大的 DDR buffer 地址间隔；
2. 连续跑 1008 full-block 和 1024 boundary-block 两个 case；
3. 用可公式化生成的数据，避免 PS 端 golden 也做完整 O(N^3) 计算；
4. 仍然逐元素检查 C 矩阵，mismatch_count 必须为 0。
```

DDR 地址规划是：

```text
instr = 0x01010000
A     = 0x02000000
B     = 0x03000000
C     = 0x04000000
```

运行脚本是：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm1024_test.tcl
```

## 板级验证结果

串口输出里 `1008` case：

```text
PS-PL-DDR GEMM 1024-capable sanity start
IP base = 0x40000000
This app runs 1008 full-block and 1024 boundary-block cases.

--- GEMM large case N=1008 K=1008 M=1008 ---
instr addr = 0x01010000
A addr     = 0x02000000
B addr     = 0x03000000
C addr     = 0x04000000
AP_CTRL before start = 0x00000004
AP_CTRL after wait = 0x00000006
done=1 idle=1 ready=0
timer_ticks = 621193433
time_us = 1863580
ap_return = 1
checksum32 = 0xE4BC7045
mismatch_count = 0
GEMM large case PASS
```

`1024` case：

```text
--- GEMM large case N=1024 K=1024 M=1024 ---
instr addr = 0x01010000
A addr     = 0x02000000
B addr     = 0x03000000
C addr     = 0x04000000
AP_CTRL before start = 0x00000004
AP_CTRL after wait = 0x00000006
done=1 idle=1 ready=0
timer_ticks = 793531675
time_us = 2380594
ap_return = 1
checksum32 = 0x58C69AF5
mismatch_count = 0
GEMM large case PASS

PS-PL-DDR GEMM 1024-capable sanity PASS
```

整理成表：

| Case | 路径含义 | time_us | checksum32 | mismatch_count | 结果 |
| --- | --- | ---: | --- | ---: | --- |
| `1008x1008x1008` | full-block 主路径 | 1863580 | `0xE4BC7045` | 0 | PASS |
| `1024x1024x1024` | boundary-block 路径 | 2380594 | `0x58C69AF5` | 0 | PASS |

这说明：

```text
PS 写 DDR 输入
  -> PL 通过 AXI master/HP 读 A/B/instruction
  -> GEMM scheduler 完成大矩阵分块
  -> PL 写回 C 到 DDR
  -> PS invalidate cache 后逐元素检查 C
```

整条链路已经在 `1008` 和 `1024` 两个大尺寸上闭环。

## 这一版我学到的东西

这一版对我最重要的收获是：HLS C-synth 通过还不够，真正能不能作为板级 IP，要继续过：

```text
Vivado implementation
timing
XSA export
Vitis platform/app
板上 PS-PL-DDR 数据闭环
```

另外这次也让我更清楚地理解了 `1008` 和 `1024` 的验证意义：

```text
1008 是为了证明 full-block 主路径；
1024 是为了证明 boundary/padding/store mask 路径。
```

如果只跑 `1024`，虽然也能说明功能正确，但不如两个 case 分开解释清楚。

这次 Vivado Tcl 自动化也很有价值。以前我通过 GUI 配过 112 工程，现在把这条流程固化成 Tcl 后，后面如果替换 HLS IP 或扩展 Conv/Attention，就不需要每次重新手点 block design，减少了很多人为配置误差。

## 后续想法

到这一版为止，我认为 GEMM 主线可以先作为稳定 baseline：

```text
TILE=14 / BLOCK=112 / MAX=1024
AXI-Lite control
AXI master / HP DDR access
64-bit instruction stream
GEMM + END
1008/1024 board validation PASS
```

接下来更合理的方向不是继续在 GEMM 上反复小改，而是把 Conv 和 Attention 接回这个已经验证过的框架：

```text
1. Conv 先做 1x1 conv via GEMM；
2. 再考虑 3x3 conv 的 im2col 或专门展开；
3. Attention 先把 QK^T 和 P*V 拆成 GEMM 子步骤；
4. 扩展 instruction opcode，让 accelerator_top 可以 dispatch GEMM / CONV / ATTN；
5. 每加一个算子，都走 C-sim -> C-synth -> Vivado -> Vitis -> board check。
```

这一版之后，我可以和老师比较明确地说：

```text
我已经把大矩阵 GEMM 从 HLS 优化走到了板级 1024 验证，
下一步不是再证明 GEMM 能跑，而是开始把 CNN/Transformer 子算子映射到这个 GEMM/instruction/AXI 框架上。
```
