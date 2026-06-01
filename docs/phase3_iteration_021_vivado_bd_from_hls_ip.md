# Phase 3 / Iteration 021：在 Vivado 中把 HLS IP 接入 Zynq PS-PL-DDR 系统

## 我这一版想解决什么

Iteration 020 已经把 Phase 3 的上板路线定下来：先不急着继续优化 GEMM，也先不加入 Conv 和 Attention，而是先证明板端系统链路能跑起来。

这一版主要记录 Vivado 这一段工作，也就是：

```text
Vitis HLS 已经导出 accelerator_top_axi IP
  -> Vivado 新建硬件工程
  -> 添加 ZYNQ7 Processing System
  -> 添加 accelerator_top_axi
  -> 连接 AXI-Lite 控制通道
  -> 连接 AXI master 到 PS DDR 高性能端口
  -> 配好 Mizar Z7 的 UART / SD / DDR
  -> 生成 bitstream
  -> 导出带 bitstream 的 XSA
```

这里不重复展开 Vitis HLS 的 C-sim、C-synth、export IP 细节。我现在更关心的是：HLS 给 Vivado 的东西是什么，Vivado 怎么把这个 IP 放进真实 Zynq 系统里。

## 这一步的整体理解

可以把这一阶段理解成在 Vivado 里画一张 SoC 系统图。

```text
Zynq PS:
  ARM CPU、DDR 控制器、PS 端 MIO、UART、SD、时钟和复位。

accelerator_top_axi:
  我用 Vitis HLS 生成的 PL 计算 IP。

DDR:
  后续放 A/B/C 矩阵和 instruction stream 的大内存。

AXI-Lite:
  PS 写控制寄存器的小通道，用来配置地址、instr_num、start、读 done/status。

AXI master + S_AXI_HP0:
  PL 自己去 DDR 读写大块矩阵数据的通道。
```

所以 Vivado 这一步不是在写 PS 端程序，而是在告诉硬件：

```text
PS 怎么控制我的 accelerator_top_axi
accelerator_top_axi 怎么访问 DDR
所有模块用哪个 clock/reset
每个 AXI 设备在地址空间里是什么门牌号
```

## HLS IP 交给 Vivado 的是什么

HLS 这边真正交给 Vivado 的不是一个普通 `.cpp` 文件，而是打包后的 IP repository。当前工程路径是：

```text
vitis_hls_project/accel_axi_o1_112/solution1/impl/ip
```

这个目录里关键文件是：

```text
component.xml
xilinx_com_hls_accelerator_top_axi_1_0.zip
hdl/verilog/accelerator_top_axi.v
drivers/accelerator_top_axi_v1_0/
```

可以这样理解：

```text
component.xml:
  IP 的身份证。Vivado 通过它知道这个 IP 名字、接口、寄存器、文件列表。

hdl/verilog/accelerator_top_axi.v:
  HLS 综合出来的 RTL。

drivers/accelerator_top_axi_v1_0:
  以后 Vitis 给 PS 端调用这个 IP 生成的裸机驱动。
```

第一次 Vivado 搜不到 `accelerator_top_axi`，就是因为 IP 没有被完整打包出 `component.xml`。当时还遇到 Vivado 2020.2 的 `core_revision` 日期数字过大问题，`run_ippack.tcl` 里生成了类似：

```text
2605312313
```

旧版 Vivado IP packager 不能接受这个 revision。处理办法是把 revision 改成小数字，例如 `1`，并且必须在 `impl/ip` 目录下运行 `run_ippack.tcl`，否则相对路径会跑到 `C:\Windows\System32\drivers` 之类的位置。

这个问题已经在 HLS 脚本里加了 workaround，后续重新导出 IP 时不用每次手改。

## Vivado 工程放在哪里

这次 Vivado 工程放在工程内部，方便后续管理：

```text
gzy_gemm_accel/vivado_board/accel_axi_112
```

这个目录本身是 GUI 生成工程，不作为源码提交。真正需要提交的是：

```text
hls/src/
hls/tb/
hls/scripts/
docs/
```

Vivado 工程更像是本地实验工作区。以后如果要复现，应优先靠 HLS source、Tcl、文档和关键配置，而不是把整个 GUI 工程塞进仓库。

## 添加 ZYNQ7 Processing System

Block Design 里先添加：

```text
ZYNQ7 Processing System
```

这个 IP 是 Vivado 自带的，不是我自己写的，也不是 HLS 导出的。它代表 Zynq-7000 里面的 PS 部分：

```text
ARM Cortex-A9
DDR Controller
M_AXI_GP0 / S_AXI_HP0
FCLK_CLK0
FCLK_RESET0_N
UART / SD / MIO / FIXED_IO
```

如果只添加 `accelerator_top_axi`，它只是一个孤立的 PL 计算模块。添加 `ZYNQ7 Processing System` 后，系统里才有 ARM 程序运行的位置，也才有 DDR 控制器和 PS-PL AXI 接口。

## Run Block Automation

添加 Zynq PS 后，Vivado 会提示：

```text
Run Block Automation
```

这一步主要把 PS 的基础外设接口接出来，常见选项是：

```text
Make Interface External: FIXED_IO, DDR
Cross Trigger In: Disable
Cross Trigger Out: Disable
```

保持默认即可。

这里的含义是：

```text
DDR:
  接到板载 DDR 颗粒。

FIXED_IO:
  接 Zynq PS 的固定 MIO 管脚、时钟、复位等。

Cross Trigger:
  用于复杂硬件调试联动，第一次 bring-up 不需要。
```

这一步完成后，PS 不再只是一个方框，而是把板子的 DDR 和固定 IO 露出来了。

## 配置 Mizar Z7 的 UART、SD 和 DDR

这一步很关键，因为后面的 Vitis platform 会完全依赖 Vivado 导出的 XSA。如果 XSA 里没有 UART，Vitis 创建 Hello World 时就会提示：

```text
This application requires a Uart IP in the hardware.
```

根据 `references/Mizar Z7用户手册_V1.1.pdf` 和 HLS 教程，Mizar Z7 需要在 Zynq PS 里配置：

```text
MIO Configuration:
  Bank1 I/O Voltage = LVCMOS 1.8V
  UART1 = MIO 48..49
  SD0 = MIO 40..45

DDR Configuration:
  Memory Type = DDR 3
  Memory Part = MT41J256M16 RE-125
```

UART1 的作用是让 PS 端程序通过 CP2102 USB-UART 打印输出：

```text
PS UART1 MIO48/49
  -> TXS0102 level shifter
  -> CP2102
  -> Micro USB UART
  -> PC 串口工具
```

SD0 只是启用 PS 的 SDIO 外设，不等于把启动模式改成 SD 卡启动。启动模式由板子上的 J1 拨码在上电时采样决定，这和 Vivado 里勾选 SD0 是两件事。

DDR 型号必须和板子匹配。这里选择 `MT41J256M16 RE-125`，是为了让 PS DDR 初始化和真实板载 DDR 一致。后面 Vitis 下载 ELF 到 DDR 或 PS/PL 共享 DDR 时，都依赖这套 DDR 配置。

## 添加 accelerator_top_axi

先在 Vivado 里添加 HLS IP repository：

```text
Tools -> Settings -> IP -> Repository -> +
```

添加：

```text
C:\Transformer\gzy_gemm_accel\vitis_hls_project\accel_axi_o1_112\solution1\impl\ip
```

然后在 Block Design 里点 `+` 搜索：

```text
accelerator_top_axi
```

这里要注意几个名字的区别：

```text
accel_axi_o1_112:
  HLS project 名字。

solution1:
  HLS solution 名字。

accelerator_top_axi:
  HLS top 函数名，也是 Vivado 里真正要添加的 IP 名。
```

Vivado 搜 IP 时搜的是 IP 名，不是 HLS 工程名。因此要搜 `accelerator_top_axi`。

## accelerator_top_axi 的接口含义

当前 IP 主要有这些接口：

```text
s_axi_control:
  AXI-Lite 控制接口。PS 通过它写寄存器、设置 DDR 地址、设置 instr_num、启动 IP、读状态。

m_axi_gmem:
  AXI master 数据接口。PL 通过它访问 DDR，读 instruction/A/B，写 C。

ap_clk:
  HLS IP 时钟。

ap_rst_n:
  低有效复位。

interrupt:
  中断输出。第一次 bring-up 可以先不接，PS 端轮询 done 即可。
```

这几个接口都来自 HLS pragma，不应该在 Vivado 里硬改。如果后续接口设计不合适，应该回到 `accelerator_top_axi.cpp` 里改 pragma 和函数参数。

## 打开 S_AXI_HP0

Zynq PS 默认常见的是 `M_AXI_GP0`，它适合 PS 去控制 PL 的 AXI-Lite 寄存器。但我们的 HLS IP 还需要自己去 DDR 读写大矩阵，所以要打开：

```text
S_AXI_HP0
```

路径：

```text
双击 processing_system7_0
  -> PS-PL Configuration
  -> AXI Non Secure Enablement
  -> HP Slave AXI Interface
  -> 勾选 S AXI HP0 interface
```

名字容易混淆，但方向可以这样记：

```text
PL 的 m_axi_gmem 是 master。
PS 的 S_AXI_HP0 是 slave。
PL master 通过 PS slave HP0 访问 DDR。
```

`M_AXI_GP0` 和 `S_AXI_HP0` 的分工是：

```text
M_AXI_GP0:
  PS -> PL，用来写控制寄存器。

S_AXI_HP0:
  PL -> DDR，用来搬大块数据。
```

## Run Connection Automation

添加 PS、HLS IP 并打开 HP0 后，Vivado 会提示：

```text
Run Connection Automation
```

这一步不是简单画线。Vivado 会自动插入必要的总线和复位模块，例如：

```text
ps7_0_axi_periph:
  把 PS M_AXI_GP0 接到 accelerator_top_axi/s_axi_control。

axi_mem_intercon:
  把 accelerator_top_axi/m_axi_gmem 接到 PS S_AXI_HP0。

rst_ps7_0_50M:
  统一处理复位。
```

当前连接关系可以概括为：

```text
processing_system7_0/M_AXI_GP0
  -> ps7_0_axi_periph
  -> accelerator_top_axi_0/s_axi_control

accelerator_top_axi_0/m_axi_gmem
  -> axi_mem_intercon
  -> processing_system7_0/S_AXI_HP0
  -> DDR

processing_system7_0/FCLK_CLK0
  -> accelerator_top_axi_0/ap_clk
  -> AXI interconnect clocks

processing_system7_0/FCLK_RESET0_N
  -> rst_ps7_0_50M
  -> accelerator_top_axi_0/ap_rst_n
```

连接完成后，绿色 Designer Assistance 条消失，说明 Vivado 认为当前可自动连接的主要接口已经处理完。

## Address Editor

接线完成后需要打开：

```text
Address Editor
```

它的作用是给 AXI 设备分配地址。没有地址，PS 写控制寄存器时不知道写到哪里，PL 访问 DDR 时也不知道地址窗口。

当前已经全部 assigned，关键地址是：

```text
accelerator_top_axi_0/Data_m_axi_gmem -> processing_system7_0/S_AXI_HP0
  Base: 0x0000_0000_0000_0000
  Range: 512M
  High: 0x0000_0000_1FFF_FFFF

processing_system7_0/Data -> accelerator_top_axi_0/s_axi_control
  Base: 0x4000_0000
  Range: 64K
  High: 0x4000_FFFF
```

这说明：

```text
0x0000_0000 到 0x1FFF_FFFF:
  HP0 能访问的 DDR 地址窗口，给 PL 的 m_axi_gmem 使用。

0x4000_0000 到 0x4000_FFFF:
  accelerator_top_axi 的 AXI-Lite 控制寄存器窗口，给 PS 端 main.c 使用。
```

以后 Vitis 生成的 `xparameters.h` 里会出现对应的基地址宏。PS 端驱动就是根据这些地址去操作 IP。

## Validate Design

执行：

```text
Validate Design
```

它检查的是系统图结构，不是 GEMM 算法正确性。主要检查：

```text
AXI master/slave 是否匹配
clock/reset 是否连接
地址是否分配
接口是否悬空
外部 DDR/FIXED_IO 是否处理
```

当前结果是：

```text
Validation successful. There are no errors or critical warnings in this design.
```

这个结果表示 Vivado 认为 PS-PL-DDR 这张硬件图纸成立，可以继续生成硬件。

## Create Output Products 和 HDL Wrapper

Block Design 保存后，Vivado 里还需要把图形化 `.bd` 变成后续综合能用的文件。

常见操作是：

```text
右键 .bd -> Generate Output Products
右键 .bd -> Create HDL Wrapper
```

`Generate Output Products` 的作用是生成 IP 相关的综合、仿真、中间文件。可以理解为把图纸拆成各个 IP 的施工材料。

`Create HDL Wrapper` 的作用是给 `.bd` 包一层 Verilog/VHDL 顶层，例如生成：

```text
design_1_wrapper.v
```

没有 wrapper，Vivado 不知道整个系统顶层是什么。选择：

```text
Let Vivado manage wrapper and auto-update
```

即可。

## Generate Bitstream

之后运行：

```text
Flow Navigator -> Generate Bitstream
```

如果 Vivado 提示先跑 synthesis 和 implementation，选择 Yes。

这一步会经历：

```text
Synthesis:
  把 RTL 转成 FPGA 资源层面的逻辑。

Implementation:
  把逻辑放置到具体 FPGA 资源上并完成布线。

Bitstream:
  生成可以下载到 PL 的 .bit 文件。
```

如果这里失败，常见原因是资源不够、时序不满足、IP 输出产品没有生成完整、板卡 part 选择不对。当前 112 版本主要用于链路验证，如果后续实现压力过大，可以先降低 `TILE` 或 block 规模，再回到更大尺寸。

## Export Hardware 和 XSA

bitstream 成功后执行：

```text
File -> Export -> Export Hardware
```

一定勾选：

```text
Include bitstream
```

导出的文件是：

```text
accel_axi_112.xsa
```

`XSA` 是 Xilinx Support Archive，也可以理解成 Vivado 交给 Vitis 的硬件交接包。里面包含：

```text
PS 配置
DDR 配置
MIO/UART/SD 配置
AXI 地址地图
IP 信息
bitstream
```

后面 Vitis 创建 platform 时，必须选择这个 `.xsa`。如果 Vivado 里改过 UART、DDR、AXI 地址或重新生成 bitstream，就要重新导出 XSA，并让 Vitis platform 使用新的 XSA。

## 启动模式和 JTAG 的关系

调试时还检查了 `references/Mizar Z7用户手册_V1.1.pdf`。Mizar Z7 的启动模式由板子上的 J1 拨码决定：

```text
Boot Mode   MIO[5]   MIO[4]
JTAG          0        0
QSPI          1        0
SD Card       1        1
```

这说明 JTAG 启动模式由硬件拨码在上电时采样，不是 Vivado 勾选 SD0 决定的。Vivado 里启用 SD0 只是启用 SDIO 外设，方便后续 SD 卡使用；它不会自动把板子启动模式改成 SD。

这次 `AP transaction timeout` 的排查里，最终确认：当 XSCT 能看到 `APU` 和两个 `ARM Cortex-A9` 正常运行时，PS 调试链路才是通的。这个问题在 Vitis 日志里表现出来，但根源属于板卡启动、复位和 JTAG 调试链路。

## 这一版完成了什么

这一版完成的是 Vivado 硬件系统集成，不是最终 GEMM 上板计算。

已经完成：

```text
[x] HLS IP 打包成功，Vivado 能识别 accelerator_top_axi
[x] 新建 Vivado 工程并添加 ZYNQ7 Processing System
[x] Run Block Automation，导出 DDR 和 FIXED_IO
[x] 按 Mizar Z7 配置 UART1、SD0、DDR
[x] 添加 accelerator_top_axi IP
[x] 打开 PS S_AXI_HP0
[x] Run Connection Automation，生成 AXI interconnect 和 reset 模块
[x] Address Editor 全部 assigned
[x] Validate Design 成功
[x] Create HDL Wrapper
[x] Generate Bitstream
[x] Export Hardware with bitstream，得到 accel_axi_112.xsa
```

## 后续还要做什么

下一步进入 Xilinx Vitis，不再是在 Vivado 里继续接线，而是写和运行 PS 端软件：

```text
1. 用 .xsa 创建 platform。
2. 在 platform 上创建 application。
3. 先跑 Hello World，验证 PS、JTAG、UART。
4. 再写 PS 端 GEMM bare-metal main.c。
5. 通过 AXI-Lite 配置 accelerator_top_axi。
6. 让 PL 通过 HP0 从 DDR 读 A/B/instr，写 C。
7. PS 读回 C，和 CPU golden 对比。
8. 串口打印 PASS / mismatch / latency。
```

这一版的结论是：Vivado 侧已经把“PS 能控制 PL、PL 能访问 DDR”的硬件图纸搭出来，并导出了 Vitis 需要的 XSA。后面真正要验证的是这张图纸在板子上能不能被 PS 软件正确驱动。
