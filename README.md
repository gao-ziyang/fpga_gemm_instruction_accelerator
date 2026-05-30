# fpga_gemm_instruction_accelerator

这是我围绕 FPGA GEMM、CNN 卷积映射、Transformer Attention 和后续指令式加速器控制做的 HLS 学习工程。

我的阶段性路线是：

```text
INT8 GEMM 微核
  -> CNN Conv2D lowering/im2col 到 GEMM
  -> Transformer QKV / QK^T / P*V
  -> 最小 micro-instruction + accelerator_top
  -> 后续再考虑 PS-PL、DDR、AXI、串口调试和上板
```

## 当前状态

已经完成并验证：

1. `INT8 x INT8 -> INT32 accumulate` 的 tiled GEMM，GEMM core 不再硬编码输出右移。
2. `GEMM_MAX_N/K/M = 16/96/96`，`GEMM_TILE = 4` 的第一版扩容。
3. 固定参数 CNN Conv2D：`Input[3,6,6]`、`Weight[4,3,3,3]`，映射为 `A[16,27] x B[27,4]`。
4. Transformer QKV projection：`X[16,96] x W[96,96]`。
5. Attention score：`Q[16,96] x K^T[96,16]`。
6. Attention no-softmax：`Score_q[16,16] x V_q[16,96]`。
7. Attention row-normalization 近似版：`P_q[16,16] x V_q[16,96]`。
8. GEMM / Conv / QKV / Attention 均完成 C-sim、C-synth、C/RTL cosim。
9. 固定 `N=16,K=96,M=96` 的 GEMM 并行规模 sweep：`TILE=4/8/12/14`，用于比较 DSP 使用、RTL latency 和理论性能 gap。
10. 新增非 AXI 版系统验证路径：`gemm_core_mac -> gemm_scheduler -> instruction/decode -> accelerator_top`。
11. 使用 `N=1024,K=1024,M=1024,TILE=12,BLOCK_N/K/M=96` 完成 V1/V2/V3 的 C-sim 和 C-synth。
12. 使用 `N=128,K=128,M=128,TILE=12,BLOCK_N/K/M=96` 完成 V1/V2/V3 的 C/RTL cosim，用于验证 RTL 等价性。
13. 完成 log11 scheduler 优化实验：64-bit 指令、`TILE=14`、`BLOCK_N/K/M=112`、A/B block 合并加载、local row banking=2、local A/B helper 等版本均完成 HLS 验证或综合记录。当前结论是 O2 有 latency 收益但 LUT 超额，O4/O5 功能正确但性能变差，后续应重点优化 bank/unroll 的资源代价和地址逻辑。
14. 新增内部 roofline 分析脚本，将 O0-O6 的 HLS 结果转成 external traffic、CTC、actual MAC/cycle、compute peak utilization 和 local feeding 模型，作为后续优化分析基线。
15. 完成 O6 full-block fast path 验证：功能、综合、Verilog cosim 均通过，但 latency 没有改善，且 full/boundary 双路径导致 DSP/LUT 明显增加，因此该版本只作为反例记录；O2 仍是性能探索点，O1 仍是当前可落地 baseline。
16. 完成 O7 row banking sweep 和 O4 inline/direct 对照：row banking=4/7 资源代价过高且 latency 退化；O4inline/O4_2 功能正确但 C-synth latency 退化到约 2979010 cycles，说明 local A/B helper 合并方向不适合继续作为落地路线。
17. 完成 O6 补充版 full-only 编译期开关验证：`O1_224_generic` 把 `N/K/M` 作为顶层运行时输入，`O6c_fullonly_224` 仍用编译期 full-only；两边功能、综合、Verilog cosim 均通过。干净对照显示 generic 边界控制主要增加 LUT/FF，而不是显著增加当前 latency。
18. 扩展 `roofline_model.py`，保留原有 internal roofline 输出，同时新增 `ideal_lower_bound_model` 和 `hls_loop_schedule_model`。新的 loop schedule 模型按当前 HLS C++ 的 `tripcount x II` 分解 latency，更适合解释 O1/O2/O4/O5 的实际差距。
19. 完成 O8 local double buffer A1 最小实验：功能和 Verilog cosim 通过，但 `load_local_a_bank/load_local_b_bank` 的 Final II 退化到 7，LUT 增加到 `86755`，latency 退化到 `705730 cycles`，因此只作为失败反例记录。路线 D 只做了设计分析，没有直接大改。

这里的 `*_top()` 都是 HLS 单元验证入口；以后真正给 `accelerator_top()` 调用的应该是 `gemm_tiled()`、`conv2d_gemm()`、`qkv_projection()`、`attention_core()` 这类 core 函数。

## 当前核心参数

| 参数 | 当前值 | 我的理解 |
| --- | --- | --- |
| `GEMM_MAX_N` | 16 | HLS 综合时数组 N 维最大容量 |
| `GEMM_MAX_K` | 96 | HLS 综合时数组 K 维最大容量 |
| `GEMM_MAX_M` | 96 | HLS 综合时数组 M 维最大容量 |
| `GEMM_TILE` | 默认 4 | 局部计算 tile，也就是当前微核并行粒度；benchmark Tcl 可以用宏临时覆盖 |
| `GEMM_BLOCK_M` | 8 | B/C 按输出列分块缓存，降低一次缓存整列 M 的压力 |
| `gemm_data_t` | `ap_int<8>` | 输入和权重量化数据 |
| `gemm_acc_t` | `ap_int<32>` | 输出和中间累加数据 |

`GEMM_MAX_*` 是硬件综合出来的最大数组容量；`N/K/M` 是每次调用时传入的实际尺寸。比如当前 `gemm_tiled()` 可以用同一份硬件跑 `7x6x5` 的 GEMM，也可以支撑 `16x96x96` 级别的 QKV。

默认功能验证仍使用 `GEMM_TILE=4`，因为它对应 16 路局部 MAC，时序压力小，适合先验证 CNN/Transformer 映射关系。后续性能 sweep 会通过 Tcl 宏临时改成 `TILE=8/12/14`，用于观察更大并行阵列下的 DSP 使用、latency 和理论性能 gap。

新增的 accelerator V1/V2/V3 路线使用独立的 `accelerator_types.h` 约束大矩阵验证规模。log10 已验证版本使用：

```text
GZY_GEMM_TILE      = 12
GZY_ACCEL_BLOCK_N  = 96
GZY_ACCEL_BLOCK_K  = 96
GZY_ACCEL_BLOCK_M  = 96
GZY_ACCEL_BENCH_N  = 1024
GZY_ACCEL_BENCH_K  = 1024
GZY_ACCEL_BENCH_M  = 1024
```

其中 `TILE=12` 对应约 144 路 MAC，`BLOCK_N/K/M=96` 对应每次搬入片上缓存的大块尺寸。

log11 scheduler 实验主要使用：

```text
GZY_GEMM_TILE                = 14
GZY_ACCEL_BLOCK_N/K/M        = 112
GZY_ACCEL_LOAD_AB_PARALLEL   = 1
GZY_ACCEL_LOCAL_ROW_UNROLL   = 2
GZY_ACCEL_LOCAL_AB_PARALLEL  = 0/1
```

指令字也从 128 bit 改成 64 bit，当前布局为 `opcode + N/K/M + base_unit`。`LOCAL_AB_PARALLEL=1` 的 helper 合并方向已经验证过功能正确，但 latency 和 LUT 都变差，所以后续不会沿着这个方向继续加码。由于当前 HLS 单元验证里 A/B/C 仍是分开的 memory port，base 字段先按 4096 element 对齐，后续真正接统一 DDR 地址空间时还需要重新设计寄存器或多条配置指令。

当前我把 GEMM core 和量化后处理分开理解：

```text
gemm_tiled()
  -> 只做原始矩阵乘和 INT32 累加
  -> C = A x B

saturate_to_int8(x, shift)
  -> 需要继续喂给 INT8 GEMM 时再做
  -> 右移 shift + 饱和到 [-128,127]
```

这样做的好处是 GEMM 单元更干净，Python/C++ baseline 可以直接按矩阵乘对齐；CNN 输出如果只是作为 INT32 特征结果，不需要被固定右移；Transformer 的 Q/K/V、Score 这类中间值需要继续参与 INT8 GEMM 时，再由每个阶段单独配置 `q_shift`、`score_shift` 或 `p_shift`。

## 工程目录

```text
gzy_gemm_accel/
  README.md
  README_conv.md
  README_attention.md
  hls/
    src/
      gemm_types.h
      gemm_core.h
      gemm_core.cpp
      gemm_top.cpp
      gemm_bench_top.h
      gemm_bench_top.cpp
      conv_types.h
      conv_core.h
      conv_core.cpp
      conv_top.h
      conv_top.cpp
      qkv_projection.h
      qkv_projection.cpp
      qkv_top.cpp
      attention_core.h
      attention_core.cpp
      attention_top.cpp
      accelerator_types.h
      gemm_scheduler.h
      gemm_scheduler.cpp
      gemm_scheduler_top.cpp
      accelerator_instruction.h
      accelerator_instruction.cpp
      instruction_decode_top.cpp
      accelerator_top.h
      accelerator_top.cpp
    tb/
      tb_gemm.cpp
      tb_gemm_bench.cpp
      tb_conv.cpp
      tb_qkv.cpp
      tb_attention.cpp
      accel_tb_common.h
      tb_gemm_scheduler.cpp
      tb_instruction_decode.cpp
      tb_accelerator_top.cpp
    scripts/
      run_hls_gemm.tcl
      run_hls_gemm_benchmark_sweep.tcl
      run_hls_conv.tcl
      run_hls_qkv.tcl
      run_hls_attention_score.tcl
      run_hls_attention_no_softmax.tcl
      run_attention_hls.tcl
      run_hls_accel_v123.tcl
      run_hls_accel_v1_scheduler.tcl
      run_hls_accel_v2_decode.tcl
      run_hls_accel_v3_top.tcl
      run_hls_accel_v1_scheduler_cosim_small.tcl
      run_hls_accel_v2_decode_cosim_small.tcl
      run_hls_accel_v3_top_cosim_small.tcl
      run_hls_accel_log11_opt.tcl
      run_hls_accel_log11_o3_scheduler.tcl
      run_hls_accel_log11_o4_scheduler.tcl
      run_hls_accel_log11_o5_scheduler.tcl
      run_hls_accel_log13_o6_fastpath.tcl
      run_hls_accel_log14_o7_row_unroll_sweep.tcl
      run_hls_accel_log15_o4_inline_direct.tcl
      run_hls_accel_log16_o6_full_only_224.tcl
      run_hls_accel_log17_o8_local_double_buffer.tcl
  python/golden/
  python/analysis/
    roofline_model.py
  docs/
    iteration_001_minimal_gemm.md
    iteration_002_tiled_gemm.md
    iteration_003_buffer_boundary_quant.md
    iteration_004_qkv_projection.md
    iteration_005_conv2d_gemm.md
    iteration_006_attention.md
    iteration_007_expand_16_96_96.md
    iteration_008_conv_init_optimization.md
    iteration_009_gemm_benchmark_sweep.md
    iteration_010_accelerator_v1_v2_v3.md
    iteration_011_scheduler_optimization.md
    iteration_012_internal_roofline_model.md
    iteration_013_full_block_fast_path.md
    iteration_014_o4_inline_direct.md
    iteration_015_o6_full_only_224.md
    iteration_016_o8_local_double_buffer.md
    teacher_feedback_roofline_next_plan.md
  reports/
    internal_roofline_points.csv
    internal_roofline_summary.md
  vitis_hls_project/   # Vitis HLS 生成目录，本地保留，不上传
```

## 主要模块

| 模块 | 作用 | 数学含义 |
| --- | --- | --- |
| `gemm_tiled` | 通用 INT8 GEMM 核 | `C = A x B`，输出为 INT32 累加结果 |
| `conv2d_gemm` | Conv2D lowering/im2col 后调用 GEMM | `Conv2D -> A x B -> output` |
| `qkv_projection` | 复用同一个 X，依次计算 Q/K/V | `Q=XWq, K=XWk, V=XWv` |
| `attention_score_core` | 计算 Attention score | `Score = Q_q x K_q^T` |
| `attention_no_softmax_core` | 暂不做 softmax，先验证完整矩阵流 | `Out = Score_q x V_q` |
| `attention_core` | row-normalization 近似 attention | `Out = P_q x V_q` |
| `gemm_core_mac` | 最底层局部 MAC 阵列 | `localC += localA x localB` |
| `gemm_scheduler` | 大矩阵分块、片上缓存和 partial sum 管理 | `A/B/C block -> local tile -> MAC array` |
| `execute_instruction_stream` | 指令译码和算子 dispatch | `GEMM instruction -> gemm_scheduler` |
| `accelerator_top` | 当前非 AXI 版系统验证顶层 | `instr_mem + A/B/C memory -> status` |

`Q/K/V` 先由 GEMM 得到 `gemm_acc_t`，后续再喂给 GEMM 前必须用 `saturate_to_int8()` 做右移、截断和饱和，转回 `gemm_data_t`。

## 如何运行

Python baseline：

```bash
python3 python/golden/gemm_4x4_baseline.py
python3 python/golden/qkv_projection_baseline.py
python3 python/analysis/roofline_model.py
```

Windows / Vitis HLS 2020.2：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_conv.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_qkv.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_score.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_attention_no_softmax.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_attention_hls.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm_benchmark_sweep.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_v123.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log11_opt.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log13_o6_fastpath.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log14_o7_row_unroll_sweep.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log15_o4_inline_direct.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log16_o6_full_only_224.tcl
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_accel_log17_o8_local_double_buffer.tcl
```

常规单元脚本会依次执行：

```text
csim_design
csynth_design
cosim_design -rtl verilog
```

`run_hls_accel_v1/v2/v3_*1024` 脚本只跑 `C-sim + C-synth`，因为 `1024^3` 的 RTL cosim 会仿真数千万周期；对应的 RTL 等价验证使用 `*_cosim_small.tcl` 在 `128^3` 规模下完成。

## 验证结果

| Case | 维度 | C-sim | C-synth | C/RTL cosim | mismatch | max_abs_error | checksum |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GEMM | `A[7,6] x B[6,5]` | PASS | PASS | PASS | 0 | 0 | 56726 |
| Conv2D via GEMM | `Input[3,6,6]`, `Weight[4,3,3,3]`, `A[16,27] x B[27,4]` | PASS | PASS | PASS | 0 | 0 | -72952 |
| QKV projection | `X[16,96] x W[96,96]` | PASS | PASS | PASS | 0 | 0 | 265116672 |
| Attention score | `Q[16,96] x K^T[96,16]` | PASS | PASS | PASS | 0 | 0 | -31663104 |
| Attention no-softmax + row-normalization | `Score_q[16,16] x V_q[16,96]` / `P_q[16,16] x V_q[16,96]` | PASS | PASS | PASS | 0 | 0 | 74785584 |
| GEMM benchmark sweep | `A[16,96] x B[96,96]`, `TILE=4/8/12/14` | PASS | PASS | PASS | 0 | 0 | 101159936 |
| Accelerator V1/V2/V3 large | `A[1024,1024] x B[1024,1024]`, `TILE=12,BLOCK=96` | PASS | PASS | 128 规模 PASS | 0 | 0 | 2087749971 |
| Accelerator log11 O0/O1/O2/O5 | `A[128,128] x B[128,128]`, `TILE=14,BLOCK=112` | PASS | PASS | PASS | 0 | 0 | 35200361 |
| Accelerator log15 O4inline/O4_2 | `A[128,128] x B[128,128]`, `TILE=14,BLOCK=112` | PASS | PASS | O4inline PASS，O4_2 未完整跑 | 0 | 0 | 35200361 |
| Accelerator log16 O1 generic/O6 full-only | `A[224,224] x B[224,224]`, `TILE=14,BLOCK=112` | PASS | PASS | PASS | 0 | 0 | -159053159 |
| Accelerator log17 O8 local double buffer A1 | `A[128,128] x B[128,128]`, `TILE=14,BLOCK=112` | PASS | PASS | PASS | 0 | 0 | 35200361 |

## 综合与 cosim 摘要

| Top | BRAM_18K | DSP | FF | LUT | Estimated clock | RTL latency |
| --- | --- | --- | --- | --- | --- | --- |
| `gemm_top` | 2 | 16 | 3914 | 5226 | 7.142 ns | 544 cycles |
| `conv_top` | 15 | 16 | 1894 | 3574 | 7.103 ns | 2594 cycles |
| `qkv_top` | 2 | 16 | 3921 | 5375 | 7.300 ns | 360362 cycles |
| `attention_score_top` | 10 | 17 | 4517 | 5764 | 7.050 ns | max 23025 cycles |
| `attention_no_softmax_top` | 37 | 49 | 13135 | 18117 | 7.300 ns | max 407139 cycles |
| `attention_top` | 37 | 49 | 15884 | 20155 | 7.300 ns | max 408064 cycles |
| `gemm_scheduler_top` V1, 1024 | 48 | 144 | 22842 | 15967 | 7.111 ns | 47393183 cycles |
| `instruction_decode_top` V2, 1024 | 48 | 147 | 24629 | 17599 | 7.653 ns | 动态指令 top，最坏 latency 不作为性能值 |
| `accelerator_top` V3, 1024 | 48 | 147 | 24629 | 17612 | 7.653 ns | 动态指令 top，最坏 latency 不作为性能值 |
| log11 O0 scheduler | 56 | 196 | 33470 | 49206 | 7.218 ns | 381634 cycles |
| log11 O1 loadAB | 56 | 196 | 33282 | 49023 | 7.165 ns | 381634 cycles |
| log11 O2 loadAB+bank2 | 84 | 196 | 34317 | 67546 | 7.143 ns | 317122 cycles |
| log11 O5 localAB helper | 56 | 196 | 40296 | 83514 | 7.165 ns | 721602 cycles |
| log13 O6a full-block fast path | 56 | 392 | 62788 | 69099 | 7.218 ns | 381634 cycles |
| log13 O6b full-block fast path + bank2 | 84 | 392 | 64184 | 134637 | 7.143 ns | 317186 cycles |
| log14 O7a row bank4 | 224 | 196 | 42468 | 82719 | 7.246 ns | 330946 cycles |
| log14 O7b row bank7 | 392 | 196 | 267912 | 193181 | 7.143 ns | 413890 cycles |
| log15 O4inline helper | 56 | 196 | 43062 | 27623 | 7.165 ns | 2988226 cycles |
| log16 O1 224 generic | 56 | 199 | 35271 | 51289 | 7.653 ns | 381879 cycles |
| log16 O6c full-only 224 | 56 | 196 | 29822 | 19539 | 7.263 ns | 381634 cycles |
| log17 O8a local double buffer A1 | 56 | 196 | 46991 | 86755 | 7.165 ns | 705730 cycles |

`attention_top` 里 row-normalization 目前使用整数除法，HLS 生成了 `sdiv`，所以它是一个能跑通的第一版近似，不是最终资源优化版本。

V2/V3 的顶层 `N/K/M` 来自指令字段，HLS 综合报告会给出非常保守的动态最坏 latency；当前性能分析主要看 V1 固定尺寸 scheduler 的 `47393183 cycles`。V2/V3 用 128 规模 RTL cosim 验证控制路径和 RTL 等价性，Verilog latency 分别为 `315251 cycles`。

log11-log17 的几个小实验说明：`TILE=14` 可以把 DSP 提到 196 个，接近 ZYNQ-7020 上限；A/B block 合并加载本身没有降低 latency；row banking=2 能把 128 规模 RTL latency 从 `381634` 降到 `317122`，但 LUT 超过器件容量。O7 继续把 row banking 加到 4/7 后，BRAM/LUT 明显爆炸且 latency 退化，所以行方向 banking 不适合作为落地路线。local A/B helper 合并方向也已经收敛：O4/O5 慢，O4inline/O4_2 直接退化到约 `2979010` C-synth cycles。O6 full-only 证明编译期只保留 full path 是可行的；和 `O1_224_generic` 对比时，LUT 从 `51289` 降到 `19539`，说明边界/generic 控制对资源代价很大，但 latency 只从 `381879` 降到 `381634`。O8a 说明顺序 ping-pong local double buffer 没有形成 load/compute overlap，动态 bank helper 还让 local A/B load Final II 退化到 7，LUT 到 `86755`。当前性能瓶颈仍然主要在 block/tile 调度和 local feeding 没有重叠，后续优化重点不是盲目加 pragma，而是更谨慎地验证静态 ping-pong/dataflow 是否真的 overlap。

## 论文启发和内部 roofline

结合 DianNao 和 FPGA'15 CNN accelerator 论文，我现在把性能问题拆成两层：

```text
外部 roofline:
  DDR/AXI -> A_buf/B_buf/C_buf 的数据搬运是否限制整体吞吐。

内部 roofline:
  A_buf/B_buf/C_buf -> localA/localB/localC -> GEMM MAC 阵列是否喂得上。
```

当前阶段重点先看内部 roofline。原因是 O1 合并外层 A/B block load 没有降低 latency，而 O2 的 row banking/unroll=2 能降低 latency，说明瓶颈更像是片上 buffer 到 local tile 的喂数路径。

`python/analysis/roofline_model.py` 会生成：

```text
reports/internal_roofline_points.csv
reports/internal_roofline_summary.md
reports/ideal_lower_bound_points.csv
reports/ideal_lower_bound_summary.md
reports/hls_loop_schedule_points.csv
reports/hls_loop_schedule_summary.md
reports/combined_roofline_points.csv
reports/combined_roofline_summary.md
```

现在我把模型分成三层来看：

```text
ideal_lower_bound_model
  -> DDR 带宽下界 + 理想 compute + 理想 local 搬运
  -> 只作为 optimistic lower bound，不预测 HLS latency

hls_loop_schedule_model
  -> 按当前 C++ loop tripcount x II 估算
  -> 对应 T_load_AB_block + T_compute_block_internal + T_store_C_block + T_control

internal_roofline_model
  -> 保留旧的 roofline / CTC / resource-efficiency 口径
```

当前 O0-O5 的 roofline 核心结果：

| Case | attainable MAC/cycle | actual MAC/cycle | compute util | attainable util | latency/roof | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| O0 | 128.000 | 5.495 | 2.80% | 4.29% | 23.29x | 串行调度基线，internal/scheduler-bound |
| O1 | 128.000 | 5.495 | 2.80% | 4.29% | 23.29x | A/B block 合并加载无收益 |
| O2 | 128.000 | 6.613 | 3.37% | 5.17% | 19.36x | row banking 有收益，但 LUT 超额 |
| O4/O5 | 128.000 | 2.906 | 1.48% | 2.27% | 44.04x | local A/B helper 合并方向退化 |

这里的 `attainable MAC/cycle = min(compute roof, memory roof)`。在当前假设下，`compute roof = 196 MAC/cycle`，外部 DDR roof 约为 `128 MAC/cycle`，所以 attainable roof 是 `128 MAC/cycle`。O2 只有 `6.613 MAC/cycle`，说明当前不是先被 DDR 卡死，而是 PL 内部 scheduler/local feeding 明显没喂满 MAC 阵列。

## Latency 分解和各版本归类

我现在采用下面这套更贴近当前 HLS 代码的分解：

```text
T_total
  ~= T_load_AB_block
   + T_compute_block_internal
   + T_store_C_block
   + T_control
```

其中 `T_load_AB_block` 对应 DDR/memory port 到 `A_buf/B_buf` 的大块加载循环；`T_compute_block_internal` 对应 `compute_block()` 里的 localC 读取、localA/localB 读取、`gemm_core_mac()` 和 localC 写回；`T_store_C_block` 对应 `C_buf -> C_mem`；剩下没有被 tripcount x II 解释掉的部分记作 `T_control`。这个模型不使用 tail block 的实际 `current_N/K/M` 去减少循环次数，因为当前 generic HLS 代码的循环上界仍然是 `ACCEL_BLOCK_N/K/M`。

| Version | 图中主分类 | HLS loop model | HLS actual | T_control | ideal roof gap | deploy | 我的理解 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| O0 | `T_compute_block_internal` | 336896 | 381634 | 44738 | 23.29x | deployable | 串行/合并外部加载在当前规模差异不明显，主要仍卡在内部 local feeding。 |
| O1 | `T_compute_block_internal` | 336896 | 381634 | 44738 | 23.29x | deployable | 当前 ZYNQ-7020 可落地 baseline，LUT=49023 未超过 53200。 |
| O2 | `T_compute_block_internal` | 272384 | 317122 | 44738 | 19.36x | not deployable | row_unroll=2 降低 local load/store 时间，所以更快；但 LUT=67546 超过 53200，只能作为性能探索点。 |
| O4 | `T_compute_block_internal` | 623616 | 721602 | 97986 | 44.04x | not deployable | local A/B helper 让每个 output tile 的 local 搬运变重，latency 和 LUT 都退化。 |
| O5 | `T_compute_block_internal` | 623616 | 721602 | 97986 | 44.04x | not deployable | 加 helper buffer partition 没救回来，说明问题不是单纯 pragma 缺失。 |
| O4inline | `T_control` | 623616 | 2988226 | 2364610 | 182.39x | deployable | 强制 inline 后 LUT 降了，但控制/调度开销极大，latency 失控，不适合作为路线。 |
| O6a | `T_compute_block_internal` | 336896 | 381634 | 44738 | 23.29x | not deployable | runtime full/fallback 双路径一起综合，DSP/LUT 翻倍，没有 latency 收益。 |
| O6b | `T_compute_block_internal` | 272384 | 317186 | 44802 | 19.36x | not deployable | 继承 O2 的 latency 收益，但又叠加 O6 双路径资源问题。 |
| O1_224_generic | `T_compute_block_internal` | 336896 | 381879 | 44983 | 6.66x | deployable | 运行时 `N/K/M` generic 版本接近以后 DDR 场景，资源接近上限但仍未超过 LUT。 |
| O6c_fullonly_224 | `T_compute_block_internal` | 336896 | 381634 | 44738 | 6.66x | deployable | full-only 大幅降低 LUT，但 latency 基本不变，说明边界判断主要是资源问题。 |
| O7a | `T_load_AB_block` | 244736 | 330946 | 86210 | 20.20x | not deployable | row_unroll=4 继续降低内部 local 部分，但 BRAM/LUT 代价过高，外层 load 和控制变得更显眼。 |
| O7b | `T_control` | 226304 | 413890 | 187586 | 25.26x | not deployable | row_unroll=7 资源和控制复杂度爆炸，latency 反而退化。 |
| O8a | `T_compute_block_internal` | 1025024 | 705730 | -319294 | 43.07x | not deployable | 顺序 ping-pong local double buffer 没有 overlap，local A/B load II=7，local feeding 占比约 93.44%，latency 和 LUT 都退化。 |

对照理想下界看，128 规模版本的 `ideal_roofline_cycles` 只有 `16384`，即使采用五阶段理想不 overlap 的 `ideal_no_overlap_cycles` 也只有 `82944`。O1 实际是 `381634`，约为理想 roofline 的 `23.29x`、五阶段理想的 `4.60x`。O8a 实际是 `705730`，约为理想 roofline 的 `43.07x`、五阶段理想的 `8.51x`，说明错误形态的 double buffer 会把 local feeding 问题放大。整体来看，当前主要差距不是“理论 MAC 数不够”，而是 HLS loop schedule 里 local load/store、block 级调度和控制没有被 dataflow/double buffer 隐藏。

模型现在还额外输出：

```text
compute_roof_mac_per_cycle / compute_roof_ops_per_cycle
mem_roof_mac_per_cycle / mem_roof_ops_per_cycle
actual_mac_per_cycle / actual_ops_per_cycle
roof_cycles_min
latency_over_roof_lower_bound
compute_peak_util
attainable_roof_util
local_model_gap
total_model_gap
GOPS/DSP, GOPS/BRAM18K, GOPS/kLUT
```

下一步优化按这个顺序推进：

```text
O6: full-only 编译期路径已验证，记录为补充反例。
O7: row banking sweep 已验证，行 banking 不作为落地路线。
O8: local double buffer A1 已验证，作为失败反例记录。
O9: 如果继续路线 A，应先尝试静态 ping/pong 或小规模局部 DATAFLOW，避免动态 bank index；一旦 LUT 超过 53200 就停止。
O10: 路线 D 先保持设计分析和小规模 prototype，不在 `TILE=14,BLOCK=112` 上直接做完整 block-level DATAFLOW。
```

## 路线 A/D 当前结论

路线 A 的最小 O8a 已经说明：只加顺序 ping-pong buffer 不够，HLS 没有形成 load next 与 compute current 的真实 overlap；动态 `bank` 参数还会引入 mux 和端口冲突，把 local A/B load 的 Final II 推到 7。后续如果继续 A2，应该避免动态 bank index，优先尝试静态 ping/pong 函数或小规模局部 DATAFLOW，并且只在 report 明确显示 overlap 时才继续。

路线 D 暂时只分析，不直接大改。当前可 overlap 的层级有三类：K tile 级 local load/compute overlap、K block 级 load next A/B block 与 compute current block、N/M block 级 load/compute/store overlap。`A_buf/B_buf` 跨 block 依赖较少，可以 ping-pong；`C_buf` 有跨 K block 的累加 RAW 依赖，必须等最后一个 K block 后才能 store，若做 block-level DATAFLOW 通常需要 C buffer ping-pong 或严格分阶段。

资源上，O1 已经是 `56 BRAM18K / 49023 LUT`，完整复制 A/B/C block buffer 会明显增加 BRAM，DATAFLOW 控制、FIFO、mux 和边界逻辑还会推高 LUT。O8a 只加 local ping-pong helper 就到 `86755 LUT`，所以在 `TILE=14,BLOCK=112` 上直接做完整 D 路线大概率无法保持 `LUT<53200`。更合理的做法是先用小规模 prototype 验证 dataflow report，再决定是否移植到主配置。

## GEMM 并行规模 sweep

这一组实验固定 `N=16,K=96,M=96`，也就是 `total MAC = 147456`，只改变 `GEMM_TILE` 和对应的 `GEMM_BLOCK_M`。实际吞吐使用 C/RTL cosim 的 Verilog latency 计算：

```text
actual_mac_per_cycle = total_mac / rtl_latency
GMAC/s @100MHz = actual_mac_per_cycle * 0.1
GOPS @100MHz = GMAC/s * 2
```

| TILE | BLOCK_M | DSP | BRAM_18K | FF | LUT | Estimated clock | RTL latency | Actual MAC/cycle | GMAC/s @100MHz | GOPS @100MHz |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 4 | 8 | 16 | 2 | 1599 | 1552 | 7.136 ns | 118720 | 1.242 | 0.124 | 0.248 |
| 8 | 8 | 64 | 2 | 5448 | 3441 | 7.136 ns | 54784 | 2.692 | 0.269 | 0.538 |
| 12 | 12 | 144 | 2 | 11865 | 6824 | 7.136 ns | 52972 | 2.784 | 0.278 | 0.557 |
| 14 | 14 | 197 | 2 | 16271 | 9393 | 7.136 ns | 54492 | 2.706 | 0.271 | 0.541 |

这组结果说明 DSP 使用量确实能随 `TILE*TILE` 增大，但实际吞吐没有线性提升。主要原因是当前 latency 统计的是完整 `gemm_tiled()` 调用，包括 A/B 搬入片上缓存、local tile 搬运、边界处理和写回；这些阶段还没有和 `dot_k` 计算阶段做 dataflow overlap。`TILE=14` 已经使用 197 个 DSP，接近 ZYNQ-7020 的 DSP 上限，适合作为“接近吃满 DSP”的性能对比点，但后续系统集成时仍需要给 Conv/Attention 外围逻辑预留资源。

Conv2D 这里已经做了两步优化：先去掉 wrapper 里对 `A/B/C` 最大矩阵的全量初始化，再把 `conv_top()` 外部接口改成 flat `input[108]`、`weight[108]`、`output[64]`，并用自增地址写 im2col/weight flatten/reshape。这样避免 HLS 为多维数组和非 2 的幂循环拍平生成大量 `urem/div/mul` 地址逻辑，Conv RTL latency 从 `15448 cycles` 降到 `2594 cycles`，额外 DSP 也从 36 降回 GEMM 微核本身的 16。

## 环境

| 项目 | 当前值 |
| --- | --- |
| OS | WSL2 Ubuntu on Windows |
| Vitis HLS / Vivado | 2020.2 |
| Target FPGA | `xc7z020clg400-2` |
| Target clock | 10 ns |
| C++ compiler | Vitis HLS 自带 GCC |
| Python | 3.12.3 |
| 当前未使用 | PyTorch、Verilator、cocotb、上板 Vivado block design |

## 后续路径思考

我一开始会把任务 2 想得像“做一个 CPU 或 RISC-V”，现在更准确的理解是：先做一个面向神经网络算子的 micro-instruction 控制器。也就是固定宽度指令字里放 `opcode`、shape、scale、buffer id 等字段，然后 `accelerator_top()` 负责取指、译码、配置参数和调用 `gemm_tiled/conv2d_gemm/qkv_projection/attention_core`。

后续上板时，我也想过能不能 PC 直接串口连 PL 顶层。现在我的理解是：不太建议这样做。串口通常先进 PS 或外部 USB-UART 控制逻辑，再由 PS 通过 AXI-Lite/AXI Master/DMA 去驱动 PL 加速器。PL 更适合做确定的数据通路和计算阵列；PS 更适合做串口协议、DDR 管理、启动停止和状态读取。

所以我后面的优先级是：

```text
1. 保持各算子 core 稳定。
2. 做 C-sim 友好的 accelerator_top 指令解释器。
3. 再把最终 accelerator_top 的接口改成 AXI-Lite 控制 + DDR/AXI/DMA 数据通路。
4. 最后再考虑串口作为 PC 与 PS 的调试入口。
```
