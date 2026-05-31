# Phase 3 / Iteration 020：上板闭环开启，先跑通 PS-PL-DDR-GEMM

## 我这一版想解决什么

前面 Phase 1 已经把 GEMM、Conv、Attention 和非 AXI 版 `accelerator_top` 跑通；Phase 2 主要围绕 O1/O2/O8/DATAFLOW 做资源、latency 和 ZYNQ-7020 约束分析。现在 Phase 3 的目标先不继续堆优化，而是开始上板闭环。

这一版的第一次上板目标是：

```text
PS 生成 A/B 和 golden C
  -> A/B 写到 DDR
  -> PS 通过 AXI-Lite 配置 accelerator_top_axi
  -> PL 通过 AXI master 从 DDR 读 A/B 和指令
  -> PL 内部 decode 指令，调用 GEMM scheduler + O1 baseline
  -> PL 写 C 回 DDR
  -> PS 读 C，对比 CPU baseline
  -> 串口打印 PASS / mismatch / latency
```

第一次只做 GEMM。Conv 和 Attention 暂时不加入，核心计算也先不继续优化，先用 O1 baseline 或资源更稳的 O1-like 配置完成板端闭环。

## 第一次上板不做什么

```text
不加入 Conv 指令
不加入 Attention 指令
不做完整 DATAFLOW / Route D
不追求 1024 或 4096 的性能数字
不做 SD 卡 / QSPI 固化
```

第一次只用 JTAG 调试：

```text
Vitis Program FPGA  -> 下载 bitstream 到 PL
Run As / Launch on Hardware -> 下载 main.elf 到 PS 并运行
串口查看 PASS / FAIL / mismatch / latency
```

断电后程序会丢失，这对第一次 bring-up 是可以接受的。后面需要脱离电脑启动时再做：

```text
BOOT.BIN = FSBL + bitstream + main.elf
```

## 当前指令怎么用

当前已有的指令层是 Phase 3 的第一版基础。现在代码里的指令字是 64 bit：

```text
[7:0]    opcode
[19:8]   N - 1
[31:20]  K - 1
[43:32]  M - 1
[49:44]  A base unit
[55:50]  B base unit
[61:56]  C base unit
[63:62]  reserved
```

当前 opcode：

```text
0 = END
1 = GEMM
```

Phase 3 第一次上板使用一条 GEMM 指令加一条 END 指令：

```text
instr[0] = GEMM(N, K, M, A_base, B_base, C_base)
instr[1] = END
```

PS 端负责生成或打包指令字，把指令写入 DDR 或 AXI-Lite 暂存区。PL 端负责读取指令、解析 opcode 和字段，然后调用对应的硬件模块。第一次只解析 `GEMM` 和 `END`。

这里要区分当前 HLS 单元验证里的 `base` 和真实上板后的 DDR 地址。

当前 64-bit 指令里的：

```text
A base unit / B base unit / C base unit
```

只有 6 bit，再乘：

```text
ACCEL_BASE_UNIT = 4096 element
```

所以它更像是 `A_mem/B_mem/C_mem` 数组内部的 offset，不是完整 DDR 物理地址。当前 HLS 验证里 `A_mem`、`B_mem`、`C_mem` 是三个独立的 `ap_memory` 数组；真实上板后，A/B/C 通常都放在同一片 DDR 地址空间里，PS 端拿到的是 32-bit byte address，例如：

```text
A_addr = 0x10000000
B_addr = 0x11000000
C_addr = 0x12000000
```

Phase 3 第一次上板时，真实 DDR 地址优先通过 AXI-Lite 控制寄存器传给 PL：

```text
AXI-Lite:
  instr_addr
  A_addr
  B_addr
  C_addr
  N/K/M
  instr_num
  start/status
```

AXI-Lite 用来传控制寄存器和地址，不用来搬完整矩阵数据。A/B/C 这类大数据放在 DDR，PL 通过 AXI master / HP 口去 DDR 读写。

## PS 和 PL 的分工

Phase 3 采用这个分工：

```text
PS:
  生成测试数据 A/B
  计算 CPU golden C
  分配 DDR buffer
  写 A/B/instruction 到 DDR
  flush / invalidate cache
  通过 AXI-Lite 配置地址、N/K/M、instr_num、start
  poll done/status
  读 C 并检查结果
  串口打印结果

PL:
  接收 AXI-Lite 控制寄存器
  通过 AXI master 访问 DDR
  读取 instruction word
  decode opcode / shape / base
  dispatch 到 GEMM / Conv / Attention 模块
  写回结果和 status
```

后续扩展 Conv 和 Attention 时，仍然保持：

```text
PS 负责生成指令流
PL 负责解析指令并调用硬件模块
```

PS 不直接替 PL 解析 opcode，也不在软件里决定每个硬件模块内部怎么调度。PS 只是把模型层或测试任务整理成指令；PL 的 decode/dispatch 才是加速器控制器。

这点对应老师说的“理解 CPU 指令集控制”。这里不是做完整 RISC-V CPU，而是做一个神经网络算子 micro-instruction 控制器：

```text
PS:
  像程序加载器 / 测试驱动

DDR:
  放数据内存，也可以放 instruction stream

accelerator_top_axi:
  像一个很小的专用处理器

execute_instruction_stream:
  取指 + PC 递增 + 遇到 END 停止

decode_instruction:
  解析 opcode / shape / buffer / flags

gemm_scheduler / conv_scheduler / attention_scheduler:
  对应不同功能单元
```

因此后续 Conv 和 Attention 写好后，PS 端不是直接替 PL 判断 opcode 并调用某个硬件模块，而是多写几条 instruction 到 DDR，PL 端按 opcode 自己 dispatch。

## 数据和指令放在哪里

Phase 3 先采用：

```text
大矩阵数据 A/B/C:
  放 DDR
  PL 通过 AXI master 读写

instruction stream:
  推荐放 DDR
  PL 通过 AXI master 读取

控制寄存器:
  放 AXI-Lite
  PS 写寄存器启动 PL
```

原因是矩阵数据会很大。比如：

```text
128 x 128 int8  A/B: 16 KB each
1024 x 1024 int8 A/B: 1 MB each
1024 x 1024 int32 C: 4 MB
```

这些数据不适合通过 AXI-Lite 一个寄存器一个寄存器写进 PL。AXI-Lite 只负责控制和地址；大块数据走 DDR + AXI master / HP 口。后续如果改成 AXI-Stream + DMA，也仍然是数据通路问题，不改变“PS 生成指令、PL 执行指令”的分工。

## 后续指令扩展顺序

第一次只保留：

```text
END
GEMM
```

GEMM 上板闭环通过后，再按这个顺序加：

```text
OP_CONV2D:
  调用 conv_scheduler()
  conv_scheduler 内部做 lowering / im2col / tile 管理
  再复用 gemm_core_mac 或 gemm_scheduler

OP_QKV:
  调用 qkv_scheduler()
  或拆成三条 GEMM micro-instruction

OP_ATTN_SCORE:
  Q x K^T

OP_ATTN_SV:
  Score/P x V

OP_ATTN:
  最后再由 attention_scheduler 组合 score / normalization / SV
```

第一版不要直接做完整 Transformer 指令。先让每个 operator opcode 都能单独 C-sim、C-synth、上板对齐，再组合成更长的 instruction stream。

后续模块分层按下面理解：

```text
execute_instruction_stream()
  -> decode opcode

  GEMM:
    gemm_scheduler()

  CONV2D:
    conv_scheduler()
      -> lowering / im2col / padding / stride / tile 管理
      -> 复用 gemm_core_mac 或 gemm_scheduler

  QKV:
    qkv_scheduler()
      -> 调 3 次 GEMM
      -> 或调用已有 qkv_projection()

  ATTN:
    attention_scheduler()
      -> score GEMM
      -> normalization / softmax 近似
      -> SV GEMM
```

也就是说，指令调度不同函数；每个函数内部再决定是否复用 GEMM。Conv 可以有自己的 `conv_scheduler()`，但它不是替代指令调度层，而是被 `OP_CONV2D` 调用的功能单元。

## HLS 侧要做的 top

保留原来的非 AXI 验证 top：

```text
accelerator_top()
  instr_mem + A_mem/B_mem/C_mem ap_memory
  用于 C-sim、C-synth、C/RTL cosim
```

新增上板专用 top：

```text
accelerator_top_axi()
  AXI-Lite:
    control / start / done / status
    instr_addr
    A_addr
    B_addr
    C_addr
    N
    K
    M
    instr_num

  AXI master:
    读 instruction
    读 A
    读 B
    写 C
```

第一次可以先支持固定小规模：

```text
N/K/M = 64 或 128
instr_num = 2
instr[0] = GEMM
instr[1] = END
```

如果 O1 `TILE=14/BLOCK=112` 在实现阶段资源或时序压力过大，第一次 bring-up 可以退到：

```text
TILE=12
BLOCK_N/K/M=96
```

等 PS-PL-DDR 链路跑通后，再切回 O1 或新的 O9 候选。

## Vivado 流程

```text
1. 新建 Vivado 工程
2. 添加 Zynq Processing System
3. Run Block Automation
4. 在 PS 配置中打开：
   - M_AXI_GP0：PS 配置 PL 的 AXI-Lite
   - S_AXI_HP0：PL 访问 DDR
   - FCLK_CLK0：给 HLS IP 时钟
5. 添加 HLS 导出的 accelerator_top_axi IP
6. s_axi_control 接 PS M_AXI_GP0
7. m_axi 接 PS S_AXI_HP0，经 AXI Interconnect / SmartConnect
8. ap_clk 接 FCLK_CLK0
9. ap_rst_n 接 processor_system_reset
10. Address Editor 分配地址
11. Validate Design
12. Generate Bitstream
13. Export Hardware，生成带 bitstream 的 .xsa
```

## Vitis PS 端 main.c 流程

```text
1. 初始化 UART / platform
2. 准备 A/B/C/golden/instr buffer
3. 生成 A/B 测试数据
4. CPU 计算 golden C
5. 打包 GEMM 和 END 指令
6. flush A/B/instr cache
7. 写 accelerator_top_axi 的 AXI-Lite 寄存器
8. start
9. poll done
10. invalidate C cache
11. 对比 C 和 golden
12. 串口打印 PASS / mismatch_count / checksum / latency
```

cache 操作是必须项：

```text
Xil_DCacheFlushRange(A, size_A)
Xil_DCacheFlushRange(B, size_B)
Xil_DCacheFlushRange(instr, size_instr)
Xil_DCacheInvalidateRange(C, size_C)
```

## 第一次成功标准

第一次上板成功不以性能极限为标准，而以链路正确为标准：

```text
bitstream 可生成
Vitis app 可下载
PS 能启动 IP
IP done 能返回
C 结果和 CPU baseline 一致
UART 打印 PASS
```

记录结果时至少写：

```text
N/K/M
TILE / BLOCK_N/K/M
opcode 序列
mismatch_count
checksum
PL cycles 或 PS 计时
资源和时序是否过线
```

## 后续路线

Phase 3 的路线先按下面推进：

```text
020: 上板方案和指令/PS-PL 分工定稿
021: 新增 accelerator_top_axi，完成 HLS C-sim/C-synth
022: Vivado block design，连接 AXI-Lite + HP DDR
023: Vitis bare-metal main.c，JTAG 跑通 GEMM
024: 扩大 N/K/M，比较 O1 / TILE=12 / O9 候选
025: 增加 CONV2D opcode
026: 增加 QKV / ATTN_SCORE / ATTN_SV opcode
027: 多指令 stream 串联执行
028: SD/QSPI 固化启动
```

## 这一版结论

Phase 3 第一件事不是继续优化 GEMM core，而是把系统链路跑通。当前指令层已经够作为 GEMM 指令控制器的第一版；后续上板 top 要从 `ap_memory` 验证接口换成 `AXI-Lite + AXI master`。第一次只跑 GEMM 和 END，两条指令跑通后再扩展 Conv 和 Attention。
