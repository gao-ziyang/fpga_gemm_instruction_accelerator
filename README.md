# fpga_gemm_instruction_accelerator

这是一个面向 ZYNQ-7020 的 FPGA GEMM / instruction accelerator 学习工程。当前主线已经从 HLS C-sim/C-synth 走到板级验证：PS 通过 AXI-Lite 配置 HLS IP，PL 通过 AXI master / HP 口访问 DDR，并在板上完成 GEMM 计算。

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

## 当前板级 IP 配置

当前上板使用的 HLS AXI top 是：

```text
hls/src/accelerator_top_axi.cpp
```

对应脚本：

```text
hls/scripts/run_hls_accel_axi_112.tcl
```

关键宏：

```text
GZY_GEMM_TILE         = 14
GZY_GEMM_BLOCK_M      = 14
GZY_ACCEL_BLOCK_N     = 112
GZY_ACCEL_BLOCK_K     = 112
GZY_ACCEL_BLOCK_M     = 112
GZY_ACCEL_MAX_N       = 112
GZY_ACCEL_MAX_K       = 112
GZY_ACCEL_MAX_M       = 112
GZY_ACCEL_MAX_INSTR   = 4
GZY_ACCEL_FULL_ONLY   = 0
GZY_ACCEL_FULL_BLOCK_FAST = 0
```

注意：当前不是 `FULL_ONLY` 版本。`gemm_scheduler` 中仍然有 `current_N/current_K/current_M` 边界判断。也就是说，它不是“无边界判断，只能跑整块”的代码。

不过当前 AXI IP 的综合脚本把 `GZY_ACCEL_MAX_N/K/M` 和 m_axi depth 都设成了 `112`。所以当前 bitstream 已经严格验证的是 `112` 以内以及 `112` 这个完整 block 尺寸；不要直接把它当成已经正式支持 `1024/2048` 的最终大矩阵 IP。要做大矩阵，建议先重新导出一个大尺寸兼容的 AXI IP，至少把 `GZY_ACCEL_MAX_*` / bench 尺寸和 PS buffer 规划一起改掉。

## 目录结构

```text
gzy_gemm_accel/
  hls/
    src/                  # HLS C++ 源码
    tb/                   # HLS testbench
    scripts/              # Vitis HLS Tcl 脚本
  ps_apps/                # 可追踪的 PS 端应用源码快照
  scripts/                # XSCT 下载/运行脚本
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

## 已验证的 Vitis/XSCT 运行方式

先在 Vitis 里 build 对应 application，再 Program Device，然后用 XSCT 脚本下载 ELF。

AXI-Lite register sanity：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct_run_ip_reg_test.tcl
```

GEMM 运行脚本：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct_run_gemm_test.tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct_run_gemm16_test.tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct_run_gemm112_test.tcl
```

如果 XSCT `targets` 中出现 DAP/AP transaction error，优先按 JTAG/DAP/PS debug 状态问题处理，不要先怀疑 C 代码或 linker script。更详细记录见：

```text
docs/phase3_iteration_022_vitis_platform_application_hello_world.md
docs/phase3_iteration_023_ps_pl_ddr_gemm_sanity.md
```

## 关于 1024 / 2048 / 1008 / 2016

当前这版不是 full-only，所以从 scheduler 逻辑看，`1024`、`2048` 这种非 112 整倍数尺寸也有边界路径，不是一定要换成 `1008`、`2016`。

但从性能和排查角度，下一步建议先测 112 的整数倍：

```text
1008 = 9  * 112
2016 = 18 * 112
```

原因：

```text
1. 1008/2016 避开边界 block，更适合先测 full block 主路径。
2. 如果 1008/2016 过了，再测 1024/2048，可以单独验证边界路径。
3. 大矩阵需要重新规划 DDR buffer 地址间隔，不能继续用 0x01020000 / 0x01030000 / 0x01040000 这种 64KB 间隔。
4. 当前 AXI IP 的 MAX/depth 是 112，直接用旧 bitstream 跑 1008/2016 不够严谨；更建议重新导出大尺寸兼容 IP。
```

大矩阵 buffer 估算：

| 尺寸 | A int8 | B int8 | C int32 | 合计 |
| --- | ---: | ---: | ---: | ---: |
| `1008^3` | about 1.0 MB | about 1.0 MB | about 4.1 MB | about 6.1 MB |
| `2016^3` | about 4.1 MB | about 4.1 MB | about 16.3 MB | about 24.5 MB |

所以大矩阵 PS 端建议改成类似：

```text
instr = 0x01010000
A     = 0x02000000
B     = 0x03000000
C     = 0x04000000
```

## 下一步计划

我建议不要马上跳 `2048`。更稳的路线：

1. 给当前 `112x112x112` 加 cycle 计时，记录 PS 端等待 IP 的周期数。
2. 重复运行 `112x112x112` 多次，确认稳定性。
3. 加一条第二个 GEMM 指令，验证 `GEMM + GEMM + END` 指令流，而不是只跑单指令。
4. 测不同 `a_base/b_base/c_base`，验证多 buffer offset。
5. 重新导出大尺寸兼容 AXI IP，例如 `MAX_N/K/M=1008`，先测 `1008x1008x1008`。
6. 如果 1008 稳定，再测 `1024`，专门验证边界路径。
7. 最后再考虑 `2016/2048`，并加入运行时间、吞吐和 DDR 带宽分析。

短期最值得做的是：

```text
112 计时 + 多指令 + 多 buffer
```

这比直接堆到 2048 更能证明这个 instruction accelerator 架构是可控的。

## 迭代日志

重要日志：

```text
docs/phase3_iteration_020_board_bringup_plan.md
docs/phase3_iteration_021_vivado_bd_from_hls_ip.md
docs/phase3_iteration_022_vitis_platform_application_hello_world.md
docs/phase3_iteration_023_ps_pl_ddr_gemm_sanity.md
docs/phase3_note_hls_driver_makefile_echo_windows.md
docs/phase3_note_vivado_2020_ip_packager_revision.md
```

Phase 1/2 的历史功能、优化和反例仍保留在 `docs/phase1_*`、`docs/phase2_*` 中。README 只记录当前主线和下一步。
