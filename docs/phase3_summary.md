# Phase 3 汇总：AXI 上板、1024 GEMM 和 all-accelerator baseline

Phase 3 的主线分成两段：先把 GEMM AXI IP 真正在 ZYNQ-7020 板上跑通，再把 Conv/QKV/Attention 迁移到同一个 instruction/AXI 框架里，形成 all-accelerator baseline。

## AXI GEMM 上板主线

当前板级路径是：

```text
PS C program
  -> AXI-Lite 写 accelerator_top_axi 控制寄存器
  -> accelerator_top_axi 通过 AXI master/HP 口访问 DDR
  -> execute_instruction_stream 解码 64-bit 指令
  -> gemm_scheduler 分块计算
  -> C 写回 DDR
  -> PS 端 golden check
```

当前 `hls/src` 只保留这条主线需要的核心文件：

```text
accelerator_top_axi.cpp/.h
accelerator_instruction.cpp/.h
accelerator_types.h
gemm_core.cpp/.h
gemm_scheduler.cpp/.h
gemm_scheduler_top.cpp
gemm_types.h
```

## 已上板 baseline2

当前已上板的大矩阵 GEMM IP 是：

```text
Tcl:
hls/scripts/run_gemmscheduleronly_baseline2_tile14_axi1024_explicit_banks.tcl

HLS project:
vitis_hls_project/gemmscheduleronly_baseline2_tile14_axi1024_explicit_banks
```

关键配置：

```text
TILE = 14
BLOCK_N/K/M = 112
MAX_N/K/M = 1024
GZY_ACCEL_EXPLICIT_BANKS = 1
GZY_ACCEL_COMPUTE_PADDED_INPUTS = 1
```

它不是 full-only 版本，仍然支持 `1024` 这种最后一个 block 不满 112 的情况。

## 资源和时序

`explicit_banks` HLS 估计资源：

```text
BRAM 60
DSP  200
FF   33205
LUT  22887
```

Vivado implementation 结果：

| 资源/时序 | 结果 |
| --- | ---: |
| Slice LUTs | 22862 / 53200 = 42.97% |
| Slice Registers | 33411 / 106400 = 31.40% |
| Block RAM Tile | 30 / 140 = 21.43% |
| RAMB36E1 | 16 |
| RAMB18E1 | 28 |
| DSPs | 206 / 220 = 93.64% |
| Setup WNS | 3.217 ns |
| Hold WHS | 0.023 ns |

资源结论：GEMM-only `TILE=14` 已经把 DSP 用到很满，但 LUT/BRAM/时序是可以接受的，因此它适合作为“性能版 GEMM IP”。

## 板级结果

已验证：

| 尺寸 | 结果 | 备注 |
| ---: | --- | --- |
| 112 | PASS | 小矩阵板级 sanity |
| 1008 | PASS | `9*112`，完整块路径 |
| 1024 | PASS | 触发 partial block/boundary |

大矩阵串口结果：

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
```

## MAC/cycle 关键口径

几个层级不能混在一起：

| 层级 | 公式/来源 | MAC/cycle |
| --- | --- | ---: |
| GEMM core 理论峰值 | `14*14` | 196.00 |
| core call 有效值 | `14^3 / 18` | 152.44 |
| compute_block_banked | `112^3 / 28865` | 48.67 |
| `112` HLS syn report-based | `112^3 / 66583` | 21.10 |
| `112` RTL cosim | cosim rpt | 13.13 |
| `112` board 50MHz 换算 | AP_START 到 AP_DONE | 8.11 |
| `1008` board 50MHz 换算 | AP_START 到 AP_DONE | 11.00 |
| `1024` board 50MHz 换算 | AP_START 到 AP_DONE | 9.03 |

如果老师说“单纯用 HLS 做一个 GEMM IP，包括缓存和阵列”，最接近的口径是 `gemm_scheduler/gemm_scheduler_top` 这一层，而不是包含 instruction decode、AXI master、PS 轮询和 DDR 板级等待的完整 board 口径。

## All-accelerator baseline

后来为了迁移 Conv/QKV/Attention，加入了 operator descriptor 和共享 GEMM 结构。当前保留为第三条 canonical baseline：

```text
Tcl:
hls/scripts/run_allaccelerator_baseline_tile8_stage5.tcl

HLS project:
vitis_hls_project/allaccelerator_baseline_tile8_stage5
```

配置：

```text
TILE = 8
BLOCK_N = 16
BLOCK_K = 32
BLOCK_M = 32
split AXI bundles
QKV fused store
Conv prepacked weight
pow2 norm
serial im2col
Conv stride=1 assumption
```

结果：

```text
BRAM = 34
DSP  = 88
FF   = 24447
LUT  = 48808
clock estimate = 7.334 ns
```

`compute_block` 层：

```text
MAC = 16 * 32 * 32 = 16384
latency = 1273 cycles
MAC/cycle = 12.87
```

这个 baseline 的意义是“功能和资源基线”，不是为了和 `TILE=14` GEMM-only 版本比峰值性能。

## Conv/QKV/Attention descriptor 的当前分工

当前 all-accelerator 的思想是让不同算子复用同一套 GEMM scheduler 硬件：

```text
accelerator_top_axi
  -> execute_instruction_stream
     -> GEMM descriptor
     -> CONV2D descriptor
        -> im2col / prepacked weight / shared GEMM / output store
     -> QKV descriptor
        -> X*Wq, X*Wk, X*Wv
        -> K 按转置形式存储，供后续 QK^T 使用
     -> ATTN descriptor
        -> Q*K_T
        -> row normalize
        -> P*V
```

这里 HLS top 仍然是较大的 `accelerator_top_axi`。老师提到的“用 HLS 单纯做 GEMM IP，控制最好用 HDL 做”，对应的是未来可以把 instruction/control path 从 HLS top 中拆出去，让 HLS 只保留 GEMM datapath。

## 阶段结论

Phase 3 已经证明：

1. GEMM AXI IP 能从 C-sim/C-synth 走到 Vivado implementation、XSA 和板级 PASS。
2. `1008` full-block 与 `1024` boundary-block 都能通过 golden check。
3. `TILE=14` 是 GEMM-only 性能主线，DSP 已接近上限。
4. all-accelerator 需要更小 TILE 和共享硬件，否则 Conv/QKV/Attention 辅助逻辑会让 LUT/DSP/时序失控。
5. 后续如果想回应老师的优化关注，应该分清两条线：GEMM-only 性能 IP 继续优化 MAC/cycle，operator descriptor IP 先保证资源可放下和功能上板。
