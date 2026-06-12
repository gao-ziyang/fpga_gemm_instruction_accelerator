# docs 索引

这里保留的是后续复盘、给老师解释、继续上板和优化时真正会用到的材料。旧的逐日流水账已经合并成三个阶段汇总，避免以后在几十个半成品日志里找结论。

## 阶段汇总

| 文档 | 作用 |
| --- | --- |
| [phase1_summary.md](phase1_summary.md) | 从最小 GEMM、Conv/QKV/Attention 小验证，到早期非 AXI accelerator 的学习过程 |
| [phase2_summary.md](phase2_summary.md) | `TILE=14` GEMM scheduler 的 O0-O8 优化、roofline 分析和失败路线 |
| [phase3_summary.md](phase3_summary.md) | AXI 上板、1024 GEMM、Conv/QKV/Attention descriptor、共享 GEMM 和当前 baseline |

## 保留的问题专题

| 文档 | 作用 |
| --- | --- |
| [debug_issue_notes/gemm_latency_throughput_summary.md](debug_issue_notes/gemm_latency_throughput_summary.md) | GEMM latency、MAC/cycle、syn/cosim/board 三种口径 |
| [debug_issue_notes/latency_model_formula_explanation.md](debug_issue_notes/latency_model_formula_explanation.md) | 为什么有些 latency 是 report-based 模型，公式里的常数来自哪里 |
| [debug_issue_notes/operator_descriptor_resource_timing_optimization_notes.md](debug_issue_notes/operator_descriptor_resource_timing_optimization_notes.md) | All-accelerator/shared GEMM 的 DSP、LUT、时序风险和优化方向 |
| [debug_issue_notes/tool_flow_notes.md](debug_issue_notes/tool_flow_notes.md) | Vitis HLS、Vivado、Vitis IDE、XSCT、IP packager 的实用流程记录 |

## 保留的说明图

| 图片 | 作用 |
| --- | --- |
| [debug_issue_notes/axi_gemm_latency_model_hierarchy.png](debug_issue_notes/axi_gemm_latency_model_hierarchy.png) | GEMM latency 层级关系图 |
| [debug_issue_notes/axi_gemm_hardware_resource_hotspot.png](debug_issue_notes/axi_gemm_hardware_resource_hotspot.png) | AXI GEMM 资源热点示意 |
| [debug_issue_notes/explicit_banked_array_lut_reduction_diagram.png](debug_issue_notes/explicit_banked_array_lut_reduction_diagram.png) | explicit bank 降 LUT 的结构示意 |

## 当前三条主线

| 角色 | Tcl | HLS 工程 |
| --- | --- | --- |
| GEMM scheduler-only baseline | `hls/scripts/run_gemmscheduleronly_baseline_o1_tile14_128.tcl` | `vitis_hls_project/gemmscheduleronly_baseline_o1_tile14_128` |
| GEMM board-facing baseline2 | `hls/scripts/run_gemmscheduleronly_baseline2_tile14_axi1024_explicit_banks.tcl` | `vitis_hls_project/gemmscheduleronly_baseline2_tile14_axi1024_explicit_banks` |
| All-accelerator baseline | `hls/scripts/run_allaccelerator_baseline_tile8_stage5.tcl` | `vitis_hls_project/allaccelerator_baseline_tile8_stage5` |

## 阅读建议

如果只是准备和老师汇报，优先看：

1. [phase3_summary.md](phase3_summary.md)
2. [debug_issue_notes/gemm_latency_throughput_summary.md](debug_issue_notes/gemm_latency_throughput_summary.md)
3. [debug_issue_notes/operator_descriptor_resource_timing_optimization_notes.md](debug_issue_notes/operator_descriptor_resource_timing_optimization_notes.md)

如果要继续跑工具或重新上板，再看 [debug_issue_notes/tool_flow_notes.md](debug_issue_notes/tool_flow_notes.md)。
