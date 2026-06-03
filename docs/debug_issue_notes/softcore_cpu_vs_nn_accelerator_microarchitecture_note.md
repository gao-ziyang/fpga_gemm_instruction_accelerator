# Phase 3 Note：软核 CPU、ISA、microarchitecture 和当前神经网络加速器架构的区别

## 我为什么整理这篇

这几天我在做 `accelerator_top_axi` 的时候，里面已经出现了：

```text
instruction memory
decode opcode
dispatch GEMM
END
```

这些词很容易让我联想到 CPU、RISC-V、软核、microarchitecture。为了避免后面给老师解释时说混，我把几个概念先整理清楚。

我现在的结论是：

```text
我不是在 PL 端实现一个通用 RISC-V 软核。
我是在实现一个面向神经网络算子的专用加速器 microarchitecture。
```

它确实借鉴了 CPU 的“取指、译码、执行”思想，但指令粒度、目标和复杂度都不一样。

## CPU 架构、ISA 和 microarchitecture

平时说“CPU 架构”，经常会混着说两个层面。

第一层是 ISA，也就是 instruction set architecture，指令集架构。它规定软件能看到哪些指令、寄存器、寻址方式和执行语义。比如：

```text
RISC-V
ARM
x86
x86-64 / x64
MIPS
```

第二层是 microarchitecture，也就是微架构。它规定硬件内部怎么实现这个 ISA。比如：

```text
几级流水线
取指、译码、执行、访存、写回
寄存器堆怎么读写
ALU 怎么设计
branch 怎么处理
cache 怎么接
总线接口怎么接
```

同一个 ISA 可以有很多种 microarchitecture。比如很多 CPU 都支持 x86-64 指令集，但内部流水线、cache、执行单元和调度策略可能完全不同。

## 常见 CPU / 软核例子

在 FPGA 上确实可以实现软核 CPU。常见例子有：

```text
RISC-V soft core
MicroBlaze
Nios II
OpenRISC
MIPS-like toy CPU
```

其中 RISC-V 最适合学生学习，因为它是开放指令集，文档清楚，指令编码也比 x86/x64 简洁。

Zynq-7020 本身 PS 里已经有 ARM Cortex-A9，这是硬核 CPU，不是我在 PL 里写出来的。它运行 Vitis 里的裸机 C 程序，负责串口、DDR 初始化、AXI-Lite 配置和启动 PL IP。

如果我真的要在 PL 里写一个 RISC-V 软核，至少要实现：

```text
PC 程序计数器
instruction fetch
instruction decode
register file
ALU
load/store
branch/jump
memory/bus interface
可能还要异常、中断、pipeline、cache
```

它执行的是很细的 CPU 指令，例如：

```text
add x1, x2, x3
lw  x4, 0(x5)
sw  x4, 0(x6)
beq x1, x2, label
jal x1, func
```

CPU 本身并不知道 GEMM、Conv、Attention 是什么。它只是通过大量细粒度指令组合出复杂程序。

## 当前加速器指令和 CPU 指令的区别

我现在的指令不是 CPU 级指令，而是算子级指令。

CPU 软核的一条指令通常做一个很小的动作：

```text
加法
访存
跳转
比较
写寄存器
```

我现在的一条 accelerator 指令做的是一个大算子：

```text
GEMM N K M A_base B_base C_base
END
```

后面可能扩展成：

```text
CONV2D input_addr weight_addr output_addr shape
ATTN q_addr k_addr v_addr out_addr shape
```

所以两者的粒度完全不同：

| 项目 | 通用 CPU 软核 | 当前神经网络加速器 |
| --- | --- | --- |
| 指令粒度 | add/load/store/branch | GEMM/CONV/ATTN |
| 目标 | 跑通用程序 | 加速神经网络算子 |
| 关键模块 | 寄存器堆、ALU、流水线、访存 | decode、scheduler、buffer、MAC array、AXI |
| 数据通路 | 标量/通用访存 | 矩阵/张量块搬运和计算 |
| 是否需要兼容 RISC-V | 需要，如果目标是 RISC-V CPU | 不需要 |
| 是否需要编译器支持 | 通常需要 | 不一定，PS 可以直接生成指令流 |

这也是我现在不应该把项目解释成“我在 PL 里写了一个 RISC-V”。更准确的是：

```text
我做了一个神经网络算子指令解释器和专用加速器数据通路。
```

## 我当前架构应该怎么表述

我现在可以把自己的架构表述成：

```text
PS 端负责准备数据、生成指令流、配置 AXI-Lite 寄存器和读取结果。
PL 端实现一个面向神经网络算子的轻量级 instruction dispatcher。
dispatcher 从 DDR 中读取 instruction stream，decode opcode，然后调用对应 scheduler。
底层 GEMM/Conv/Attention 共享或复用 GEMM scheduler 和 MAC array。
PL 通过 AXI master / HP 口访问 DDR，把输入搬进片上 buffer，计算后再写回 DDR。
```

更简洁一点可以说：

```text
这是一个 domain-specific neural-network accelerator microarchitecture。
它借鉴 CPU 的 fetch/decode/dispatch 思路，但不是通用 CPU。
它的指令粒度是 GEMM/Conv/Attention 这样的算子级指令。
```

如果给老师解释，可以用这段：

```text
我目前实现的不是通用软核 CPU，而是一个面向神经网络算子的专用加速器微架构。它通过 AXI-Lite 接收 PS 端配置，通过 AXI master 从 DDR 读取指令和矩阵数据。PL 内部有轻量级指令译码和 dispatch，根据 opcode 调用 GEMM、Conv 或 Attention scheduler。底层计算资源主要复用 GEMM scheduler 和 MAC array，这样比为每个算子单独堆一套计算阵列更节省 ZYNQ-7020 的资源。
```

## Conv / Attention 调用 GEMM scheduler 是否合理

我后续打算写：

```text
conv_scheduler
  -> 把 Conv 的语义映射成 GEMM 需要的 A/B/C、shape、stride/padding 或 im2col 关系
  -> 调用 gemm_scheduler

attention_scheduler
  -> 把 Attention 拆成 QK^T、P*V 等矩阵乘步骤
  -> 调用 gemm_scheduler
```

这并不违背任务目标。相反，这更像一个合理的神经网络加速器架构：

```text
GEMM 是共享计算后端。
Conv 和 Attention 是上层算子调度。
```

一天内为了先交差，Conv 可以先走 PS 端 im2col：

```text
PS 准备 im2col 后的 A
PS 准备 flatten 后的 B
PL 仍然调用 gemm_scheduler
```

这样能快速证明 Conv via GEMM 已经接进当前 accelerator。后面如果时间够，再把更多 im2col 或 shape 调度逻辑搬进 PL。

Attention 也可以先验证关键 GEMM 路径：

```text
Q x K^T
P x V
```

softmax 或 row normalization 可以先简化，或者先由 PS/Python golden 准备中间结果。重点是先证明 Attention 的核心矩阵乘能够复用当前统一 GEMM 后端。

## 和老师任务的关系

如果老师说“指令”“控制器”“架构”，我不应该马上理解成“写一个 RISC-V CPU”。更贴近当前任务的理解是：

```text
做一个面向神经网络算子的 micro-instruction controller。
```

也就是：

```text
fixed-width instruction word
  -> opcode + shape + address/base + scale/config
  -> accelerator_top fetch/decode
  -> scheduler dispatch
  -> AXI/DDR 数据搬运
  -> MAC array 计算
  -> status/result return
```

这和 CPU 有相似思想，但不是通用 CPU。

我的后续路线可以这样说：

```text
我会先把 GEMM、Conv、Attention 的功能闭环做出来。
其中 Conv 和 Attention 先作为更高层的 scheduler，复用底层 GEMM scheduler。
等三类功能都能在 PS-PL-DDR 上正确验证后，再集中优化 GEMM 的资源占用、latency 和 DDR 访存效率。
```

## 一句话总结

```text
RISC-V 软核是通用 CPU microarchitecture。
我现在做的是神经网络专用 accelerator microarchitecture。
它有指令译码和 dispatch，但指令粒度是算子级，不是 add/load/store 级。
```

这个表述更准确，也更贴合 Zynq 上用 HLS 做神经网络加速器的实际路线。
