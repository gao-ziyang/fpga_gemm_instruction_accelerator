# fpga_gemm_instruction_accelerator

这是一个面向 ZYNQ-7020 的 FPGA GEMM / instruction accelerator 学习工程。当前主线已经从 HLS C-sim/C-synth 走到板级验证：PS 通过 AXI-Lite 配置 HLS IP，PL 通过 AXI master / HP 口访问 DDR，并在板上完成 GEMM 计算。最新状态是 `1024` 兼容的大矩阵 AXI GEMM IP 已完成 Vivado implementation、XSA 导出和板级验证。

## 当前状态

当前已经验证到 Phase 3：

```text
PS -> DDR sanity
PS -> AXI-Lite -> accelerator_top_axi register sanity
PS -> AXI-Lite -> PL -> AXI master/HP -> DDR -> GEMM -> DDR -> PS golden check
```

板级 GEMM 已通过：

| 测试规模 | 结果 |
| --- | --- |
| `4 x 4 x 4` | PASS |
| `16 x 16 x 16` | PASS |
| `112 x 112 x 112` | PASS |
| `112 x 112 x 112` 重复计时 | PASS |
| `56 x 56 x 56` 双 GEMM 指令流 | PASS |
| `1008 x 1008 x 1008` | PASS |
| `1024 x 1024 x 1024` | PASS |

`112 x 112 x 112` 串口结果：

```text
PS-PL-DDR GEMM sanity start
shape N=112 K=112 M=112
AP_CTRL before start = 0x00000004
AP_CTRL after wait = 0x00000006
done=1 idle=1 ready=0
ap_return = 1
checksum = -37432528
mismatch_count = 0
PS-PL-DDR GEMM sanity PASS
```

这里 `mismatch_count = 0` 是关键：PL 写回 DDR 的 C 矩阵与 PS 端 CPU golden 逐元素一致。

补充验证中，当前 112 版 PS application 还完成了：

```text
112x112x112 单 GEMM 在程序内重复 3 次
XSCT repeat 脚本连续下载运行 ELF 3 次
GEMM + GEMM + END 多指令流
不同 a_base/b_base/c_base 的多 buffer offset
```

实测 `112x112x112` 单次等待 IP 的时间大约是：

```text
time_us = 3463 ~ 3466
```

`GEMM + GEMM + END` 测试里，`ap_return = 2`，并且 `multi56_c0`、`multi56_c1` 的 `mismatch_count` 都为 0。这说明当前 instruction stream 已经不只是单条 GEMM 指令能跑，而是可以连续执行两条 GEMM，并把结果写回不同 DDR buffer。

最新 `1024` 兼容 IP 的板级结果：

```text
1008x1008x1008:
time_us = 1863580
checksum32 = 0xE4BC7045
mismatch_count = 0
GEMM large case PASS

1024x1024x1024:
time_us = 2380594
checksum32 = 0x58C69AF5
mismatch_count = 0
GEMM large case PASS

PS-PL-DDR GEMM 1024-capable sanity PASS
```

## 当前板级 IP 配置

当前上板使用的 HLS AXI top 是：

```text
hls/src/accelerator_top_axi.cpp
```

当前最新大矩阵 IP 对应脚本：

```text
hls/scripts/run_hls_accel_axi_1024_explicit_banks.tcl
```

关键宏：

```text
GZY_GEMM_TILE         = 14
GZY_GEMM_BLOCK_M      = 14
GZY_ACCEL_BLOCK_N     = 112
GZY_ACCEL_BLOCK_K     = 112
GZY_ACCEL_BLOCK_M     = 112
GZY_ACCEL_MAX_N       = 1024
GZY_ACCEL_MAX_K       = 1024
GZY_ACCEL_MAX_M       = 1024
GZY_ACCEL_MAX_INSTR   = 4
GZY_ACCEL_COMPUTE_PADDED_INPUTS = 1
GZY_ACCEL_EXPLICIT_BANKS = 1
GZY_ACCEL_FULL_ONLY   = 0
GZY_ACCEL_FULL_BLOCK_FAST = 0
```

注意：当前不是 `FULL_ONLY` 版本。`gemm_scheduler` 中仍然有 `current_N/current_K/current_M` 边界判断。也就是说，它不是“无边界判断，只能跑整块”的代码。

Vivado 自动化脚本：

```text
scripts/vivado/build_accel_axi_1024_explicit_banks.tcl
```

它会复用 112 工程已经验证过的 block design 结构，把 HLS 1024 explicit-banks IP 接到 PS GP0 AXI-Lite 和 PS HP0 DDR 数据通路上，并导出：

```text
vivado_board/accel_axi_1024_explicit_banks/export/accel_axi_1024_explicit_banks.xsa
```

Vivado implementation 结果：

| 资源/时序 | 结果 |
| --- | ---: |
| Slice LUTs | 22862 / 53200 = 42.97% |
| Slice Registers | 33411 / 106400 = 31.40% |
| Block RAM Tile | 30 / 140 = 21.43% |
| DSPs | 206 / 220 = 93.64% |
| Setup WNS | 3.217 ns |
| Hold WHS | 0.023 ns |

## 目录结构

```text
gzy_gemm_accel/
  hls/
    src/                  # HLS C++ 源码
    tb/                   # HLS testbench
    scripts/              # Vitis HLS Tcl 脚本
  ps_apps/                # 可追踪的 PS 端应用源码快照
  scripts/
    xsct/                 # XSCT 下载/运行脚本
    vivado/               # Vivado batch 构建脚本
  docs/                   # 迭代日志和问题记录
  python/
    golden/               # Python golden / 小验证
    analysis/             # roofline 和分析脚本
```

`vitis_ws/`、`vivado_board/`、`vitis_hls_project/` 是本地 GUI/工具生成目录，默认不作为源码提交。

## 核心模块

| 模块 | 作用 |
| --- | --- |
| `gemm_core.cpp` | 局部 INT8 GEMM MAC tile |
| `gemm_scheduler.cpp` | 大矩阵分块、片上 buffer、load/compute/store |
| `accelerator_instruction.cpp` | 64-bit 指令译码和 dispatch |
| `accelerator_top_axi.cpp` | 板级 AXI-Lite + AXI master 顶层 |
| `ps_apps/accel_axi_112_gemm_test/helloworld.c` | 当前已验证的 PS-PL-DDR GEMM 测试源码快照 |
| `ps_apps/accel_axi_1024_gemm_test/helloworld.c` | 1008 full-block + 1024 boundary-block 大矩阵上板测试 |
| `scripts/xsct/` | XSCT 手动下载 ELF、重复运行和板级调试脚本 |
| `scripts/vivado/build_accel_axi_1024_explicit_banks.tcl` | 自动生成 1024 Vivado 工程、bitstream 和 XSA |

当前指令格式是 64 bit：

```text
bits  7:0   opcode
bits 19:8   N - 1
bits 31:20  K - 1
bits 43:32  M - 1
bits 49:44  A base / 4096
bits 55:50  B base / 4096
bits 61:56  C base / 4096
```

当前只验证了：

```text
GEMM
END
```

已经验证过的指令流包括：

```text
GEMM + END
GEMM + GEMM + END
```

## 已验证的 Vitis/XSCT 运行方式

先在 Vitis 里 build 对应 application，再 Program Device，然后用 XSCT 脚本下载 ELF。

AXI-Lite register sanity：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_ip_reg_test.tcl
```

GEMM 运行脚本：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm_test.tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm16_test.tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm112_test.tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_repeat_gemm112_test.tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm1024_test.tcl
```

如果 XSCT `targets` 中出现 DAP/AP transaction error，优先按 JTAG/DAP/PS debug 状态问题处理，不要先怀疑 C 代码或 linker script。更详细记录见：

```text
docs/phase3_iteration_022_vitis_platform_application_hello_world.md
docs/phase3_iteration_023_ps_pl_ddr_gemm_sanity.md
```

## 关于 1024 / 1008 / 后续更大尺寸

当前这版不是 full-only，所以从 scheduler 逻辑看，`1024` 这种非 112 整倍数尺寸有边界路径，不是只能跑 `1008` 这种整块尺寸。

实际验证顺序是先测 112 的整数倍：

```text
1008 = 9  * 112
```

原因：

```text
1. 1008 避开边界 block，用来验证大矩阵 full-block 主路径。
2. 1024 会触发最后的 partial block，用来验证 boundary/padding/store mask 路径。
3. 大矩阵需要重新规划 DDR buffer 地址间隔，不能继续用 0x01020000 / 0x01030000 / 0x01040000 这种 64KB 间隔。
4. 这一版已经重新导出了 MAX/depth=1024 的 AXI IP，并完成 1008/1024 板级验证。
```

大矩阵 buffer 估算：

| 尺寸 | A int8 | B int8 | C int32 | 合计 |
| --- | ---: | ---: | ---: | ---: |
| `1008^3` | about 1.0 MB | about 1.0 MB | about 4.1 MB | about 6.1 MB |
| `1024^3` | about 1.0 MB | about 1.0 MB | about 4.2 MB | about 6.3 MB |
| `2016^3` | about 4.1 MB | about 4.1 MB | about 16.3 MB | about 24.5 MB |

所以大矩阵 PS 端建议改成类似：

```text
instr = 0x01010000
A     = 0x02000000
B     = 0x03000000
C     = 0x04000000
```

## 下一步计划

当前 PS-PL-DDR 的 GEMM 闭环已经从小尺寸推进到 `1008/1024` 大矩阵，下一步应该把 Conv / Attention 迁移到这个已经验证过的 GEMM scheduler、instruction stream 和 AXI DDR 框架里。

已经完成的收尾验证：

1. 给当前 `112x112x112` 加 cycle 计时，记录 PS 端等待 IP 的周期数。
2. 重复运行 `112x112x112` 多次，确认稳定性。
3. 加一条第二个 GEMM 指令，验证 `GEMM + GEMM + END` 指令流，而不是只跑单指令。
4. 测不同 `a_base/b_base/c_base`，验证多 buffer offset。
5. 导出 `MAX_N/K/M=1024` 的 explicit-banks AXI IP。
6. 自动化 Vivado block design、implementation、bitstream 和 XSA 导出。
7. 板上验证 `1008x1008x1008` full-block 和 `1024x1024x1024` boundary-block。

后面更合理的主线是：

1. 把已有 Conv / Attention 逻辑迁移到当前 `gemm_scheduler` 和 instruction stream 体系里。
2. Conv 先做 `1x1 conv via GEMM`，再考虑 `3x3 conv + im2col`。
3. Attention 先拆成 `QK^T` 和 `P*V` 两个 GEMM 子步骤。
4. 扩展 opcode 和 decode，让顶层能 dispatch `GEMM`、`CONV2D`、`ATTN` 或更细的 Transformer 子算子。
5. PS 端再分别验证 GEMM、Conv、Attention 三类功能。
6. 功能闭环稳定后，再集中优化 GEMM 的资源、latency 和 DDR 访存效率。

现在不建议一上来就为了 `2048/2016` 继续放大矩阵。更稳的是先把 Conv/Attention 接到已经跑通的 GEMM/instruction/AXI 框架里，功能边界先清楚，优化才不会把调试问题搅在一起。

## 迭代日志

重要日志：

```text
docs/phase3_iteration_020_board_bringup_plan.md
docs/phase3_iteration_021_vivado_bd_from_hls_ip.md
docs/phase3_iteration_022_vitis_platform_application_hello_world.md
docs/phase3_iteration_023_ps_pl_ddr_gemm_sanity.md
docs/phase3_iteration_024_compute_boundary_hoist_padding_plan.md
docs/phase3_iteration_025_hls_resource_model_explicit_banking_plan.md
docs/phase3_iteration_026_axi_1024_board_validation.md
docs/debug_issue_notes/hls_driver_makefile_echo_windows_note.md
docs/debug_issue_notes/vivado_2020_ip_packager_revision_note.md
docs/debug_issue_notes/latency_model_formula_explanation.md
docs/debug_issue_notes/hardware_ascii_diagram_explanation.md
docs/debug_issue_notes/phase2_phase3_teacher_talking_points.md
```

调试问题、工具问题、说明类图片和非纯日志文档统一放在 `docs/debug_issue_notes/`。Phase 1/2 的历史功能、优化和反例仍保留在 `docs/phase1_*`、`docs/phase2_*` 中。README 只记录当前主线和下一步。
