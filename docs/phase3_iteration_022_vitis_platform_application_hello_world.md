# Phase 3 / Iteration 022：在 Vitis 中使用 XSA 创建平台、运行 Hello World，并打通 PS 调试链路

## 我这一版想解决什么

Iteration 021 已经在 Vivado 里完成了硬件系统：

```text
Zynq PS
  -> AXI-Lite 控制 accelerator_top_axi
  -> HP0 让 accelerator_top_axi 访问 DDR
  -> UART1 / SD0 / DDR 配置完成
  -> 导出带 bitstream 的 accel_axi_112.xsa
```

这一版记录 Xilinx Vitis 这一段，也就是从 Vivado 导出的 `.xsa` 进入软件开发：

```text
XSA
  -> Platform Project
  -> System Project
  -> Application Project
  -> Build
  -> Run As / Launch on Hardware
  -> XSCT 调试
  -> UART 看到 Hello World
```

这一步的目标不是验证 GEMM IP 的功能，而是先把最基础的 PS 软件运行链路跑通：

```text
JTAG 能访问 PS
ELF 能下载到 ARM
PS UART 能输出
Vitis platform 和 application 是基于正确 XSA 生成的
```

## 先把几个 Vitis 概念说清楚

### XSA 是什么

`XSA` 是 Vivado 导出的硬件说明包。它告诉 Vitis：

```text
这块硬件系统里有什么 PS 配置
DDR 怎么初始化
UART 在哪个 MIO 上
AXI 设备基地址是多少
有哪些 HLS IP 和驱动
bitstream 在哪里
```

可以把 XSA 理解成硬件交接文件。Vivado 负责硬件图纸和 bitstream，Vitis 负责基于这份硬件说明写 PS 端软件。

这次使用的新 XSA 来自：

```text
gzy_gemm_accel/vivado_board/accel_axi_112/export/accel_axi_112.xsa
```

这个 XSA 已经包含 UART1，所以后面才能创建 Hello World。

### Platform 是什么

`Platform Project` 是 Vitis 根据 XSA 生成的软件平台。它不是我的业务程序，而是板子的裸机软件基础环境。

里面会生成：

```text
BSP:
  Board Support Package，包含 startup、standalone OS、Xilinx 驱动和库。

xparameters.h:
  根据 XSA 自动生成的地址和设备宏。

system.mss:
  BSP 配置文件，例如 stdin/stdout 用哪个 UART。

FSBL:
  后续做 BOOT.BIN 固化启动时会用到，第一次 JTAG 调试可以先不深究。
```

可以把 platform 理解成：

```text
Vitis 眼里的这块板子。
```

如果 Vivado 硬件改了，尤其是 UART、DDR、AXI 地址、IP 变了，就要重新导出 XSA，并重新生成或更新 platform。

### Application 是什么

`Application Project` 才是我真正写 `main.c` 的地方。

例如：

```text
Hello World application:
  只验证 PS 程序能不能跑、UART 能不能打印。

后续 GEMM application:
  准备 A/B/instr，配置 accelerator_top_axi，等待 done，读回 C，和 golden 对比。
```

Application 依赖某个 platform。也就是说，application 不是孤立的，它必须知道自己运行在哪个硬件平台上。

### System 是什么

Vitis 里 `System Project` 是把 application、domain、platform 组织在一起的外层工程。

当前树里类似：

```text
accel_axi_112_hello_system [System]
  -> accel_axi_112_hello [Application]

accel_axi_112_uart [Platform]
```

可以这样理解：

```text
Platform:
  板子的软硬件基础说明。

Application:
  我写的 PS 端程序。

System:
  把这个 application 放到这个 platform/domain 上运行的组织壳。
```

### Domain 是什么

`Domain` 指 application 运行的软件环境。当前选择：

```text
standalone on ps7_cortexa9_0
```

意思是：

```text
OS = standalone
CPU = ps7_cortexa9_0
```

也就是裸机程序，不跑 Linux。第一次上板验证用 standalone 最直接。

## 创建和构建 platform

在 Vitis 里创建 platform：

```text
File -> New -> Platform Project
Name: accel_axi_112_uart
Create from hardware specification (XSA)
选择 Vivado 导出的 accel_axi_112.xsa
Processor: ps7_cortexa9_0
OS: standalone
Generate boot components: checked
Finish
```

这里名称用 `accel_axi_112_uart` 是为了区分旧 platform。第一次旧的 `accel_axi_112` 是基于没有 UART1 的旧 XSA 创建的，所以 Hello World 模板会提示缺 UART。新 platform 使用重新导出的 XSA，UART1 已经在硬件说明里。

创建后需要 Build Platform：

```text
右键 accel_axi_112_uart -> Build Project
```

或者：

```text
Project -> Build Project
```

`Build Project` 的意思是编译当前工程。对 platform 来说，它会生成 BSP、编译 Xilinx standalone 库和驱动。成功后能看到类似：

```text
Finished building libraries
Build Finished
```

这次还检查到 BSP 中已经有：

```text
system.mss:
  stdin = ps7_uart_1
  stdout = ps7_uart_1

xparameters.h:
  STDIN_BASEADDRESS  = 0xE0001000
  STDOUT_BASEADDRESS = 0xE0001000
  XPAR_PS7_UART_1_BASEADDR = 0xE0001000
```

这说明 Vitis 已经知道串口输出走 PS UART1。

## 创建 Hello World application

在 Vitis 里创建 application：

```text
File -> New -> Application Project
选择 accel_axi_112_uart platform
Application project name: accel_axi_112_hello
System project: Create new
Processor: ps7_cortexa9_0
Domain: standalone on ps7_cortexa9_0
Template: Hello World
Finish
```

如果模板页提示：

```text
This application requires a Uart IP in the hardware.
```

说明当前选的 platform 对应 XSA 里没有 UART。这个问题不能靠改 Hello World 代码解决，要回 Vivado 配好 UART1，重新导出 XSA，再重新建或更新 platform。

创建好后，左侧工程树大致是：

```text
accel_axi_112_hello_system [System]
accel_axi_112_hello [Application]
  -> src/helloworld.c
  -> src/platform.c
  -> src/platform.h
  -> src/lscript.ld
accel_axi_112_uart [Platform]
```

其中 `helloworld.c` 是 application 代码，`platform.c/h` 是模板提供的平台初始化代码，`lscript.ld` 是链接脚本。

## Build 和 Clean 是什么

`Build Project` 是编译工程。

对 application 来说，build 会把：

```text
helloworld.c
platform.c
startup code
BSP library
linker script
```

编译链接成：

```text
accel_axi_112_hello.elf
```

`ELF` 是 ARM 端可执行文件。后面 `Launch on Hardware` 其实就是把这个 ELF 下载到 `ps7_cortexa9_0` 上运行。

`Clean Project` 是清理旧的编译产物，例如：

```text
Debug/
Release/
*.o
*.elf
```

Clean 不等于删除源码。它常用于解决旧编译缓存、旧 BSP、旧链接结果导致的奇怪问题。一般流程是：

```text
Clean Project
Build Project
Run As -> Launch on Hardware
```

对 GUI 工程来说，build/clean 是很普通的工程动作，不代表设计思路有问题。

## Run As / Launch on Hardware 是什么

右键 application：

```text
Run As -> Launch on Hardware
```

这一步会自动做几件事：

```text
连接 hw_server
通过 JTAG 连接板子
下载 bitstream 到 PL
运行 ps7_init.tcl 初始化 PS
下载 application ELF 到 ARM Cortex-A9
让 ARM 从入口地址开始运行
```

所以 `Run As` 不是简单打开一个 exe，而是通过 JTAG 把程序放到板上的 ARM 里执行。

当前 Hello World 成功时，XSCT log 里能看到：

```text
Downloading Program -- C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_hello/Debug/accel_axi_112_hello.elf
section, .text: 0x00000000 - 0x00000a1f
...
Setting PC to Program Start Address 0x00000000
Successfully downloaded ...
Info: ARM Cortex-A9 MPCore #0 (target 2) Running
```

串口工具看到：

```text
Hello World

Successfully ran Hello World application
```

这说明 PS 端裸机程序已经跑起来，并且 UART 输出正常。

## Console、Vitis Log 和 XSCT Console

Vitis 里有几个容易混的窗口：

```text
Console:
  普通构建/运行日志窗口。Build、Program Device、Launch 的输出常在这里。

Vitis Log:
  IDE 层面的操作日志。

XSCT Console:
  可以直接输入 Xilinx Tcl 命令的交互控制台。
```

`Console` 更像看日志，`XSCT Console` 才是能输入命令控制硬件的地方。XSCT 的提示符是：

```text
xsct%
```

如果看到 `xsct%`，说明它在等我输入命令。

## XSCT 是什么

`XSCT` 是 Xilinx Software Command-line Tool。它可以通过 Tcl 命令连接硬件、选择 target、读写内存、下载 ELF、运行/停止 ARM。

这次调试用到的命令包括：

```tcl
connect
targets
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
mwr 0x0 0x12345678
mrd 0x0
```

含义分别是：

```text
connect:
  连接本机 hw_server。

targets:
  列出当前 JTAG/debug 能看到的目标。

targets -set ... "*A9*#0":
  选择 ARM Cortex-A9 核 0 作为当前调试目标。

stop:
  暂停 ARM 核，方便读写内存或下载程序。

mwr:
  memory write，往指定地址写一个值。

mrd:
  memory read，从指定地址读值。
```

这次正确状态下，`targets` 输出为：

```text
1  APU
   2  ARM Cortex-A9 MPCore #0 (Running)
   3  ARM Cortex-A9 MPCore #1 (Running)
4* xc7z020
```

这说明：

```text
PL 侧 xc7z020 能看到
PS 侧 APU 能看到
ARM Cortex-A9 能被调试
```

## OCM 是什么，为什么先测它

`OCM` 是 On-Chip Memory，也就是 Zynq PS 片上 SRAM。它在地址 `0x00000000` 附近，可以在不依赖外部 DDR 的情况下做最小读写测试。

这次用：

```tcl
mwr 0x0 0x12345678
mrd 0x0
```

结果是：

```text
0:   12345678
```

这说明 ARM 调试口能访问 PS 片上内存。这个测试很有价值，因为它把问题拆小了：

```text
如果 OCM 都不能读写:
  问题在 JTAG/PS debug/reset/boot mode。

如果 OCM 能读写，但 DDR 不能:
  再查 DDR 初始化和 linker。

如果 OCM 能读写，Hello World 也能跑:
  PS/JTAG/UART 基础链路已经通。
```

这次 Hello World 的 section 也下载到了 `0x00000000` 附近，所以它实际上是一个很小的 OCM 验证程序。这是合理的第一步。后面 GEMM 应用因为 A/B/C 数据大，不能长期只靠 OCM，必须回到 DDR。

## 这次遇到的问题和排查

### 问题 1：旧 XSA 没有 UART，Hello World 无法创建

现象：

```text
This application requires a Uart IP in the hardware.
```

原因：

```text
旧 platform 使用的旧 XSA 里没有 UART1。
```

解决：

```text
回 Vivado 配置 UART1 = MIO48..49
重新 Generate Bitstream
重新 Export Hardware with bitstream
新建 accel_axi_112_uart platform
Build Platform
确认 stdin/stdout = ps7_uart_1
```

### 问题 2：HLS driver Makefile 在 Windows 下 build 失败

现象：

```text
process_begin: CreateProcess(NULL, #echo "Copying 'xaccelerator_top_axi.h' ...)
make (e=2): 系统找不到指定的文件。
```

原因：

```text
HLS 2020.2 生成的 driver Makefile 里有 #echo。
在 Windows CMD 下它没有被当成 Makefile 注释，而是被当成一个命令。
```

解决：

```text
把 generated driver Makefile 中的 #echo 行改成 @echo。
并且在 run_hls_accel_axi_112.tcl 中加入后处理 workaround。
```

这个问题修完后 platform 能成功 build。

### 问题 3：CP2102 串口驱动

现象：

```text
Windows 设备管理器里 CP2102 USB to UART Bridge Controller 有黄色感叹号。
```

原因：

```text
USB-UART 芯片需要 Silicon Labs CP210x VCP 驱动。
```

解决：

```text
安装/修复 CP210x 驱动。
设备管理器出现 Silicon Labs CP210x USB to UART Bridge 对应 COM 口。
串口工具使用 115200 8N1 no flow control。
```

这里也确认了 Mizar Z7 的 JTAG 口和 UART 口是两个不同的 Micro USB。JTAG 用于下载和调试，UART 用于 PS 程序打印。

### 问题 4：AP transaction timeout

最关键的问题是 `Launch on Hardware` 报：

```text
AP transaction timeout
```

一开始这个错误看起来像 ELF 下载失败，但进一步用 XSCT `targets` 看到：

```text
1  APU (Cannot open JTAG port: AP transaction error, DAP status 0xF0000021)
   2* ARM Cortex-A9 MPCore #0 (APB Memory access port is disabled)
   3  ARM Cortex-A9 MPCore #1 (APB Memory access port is disabled)
4  xc7z020
```

这说明：

```text
PL 侧 JTAG 能看到 xc7z020。
PS 侧 APU 没有正常打开。
ARM Cortex-A9 调试口不可访问。
```

因此当时的根因不是 Hello World 代码，也不是 UART，而是 PS/JTAG/debug 链路状态不正常。

进一步查 Mizar Z7 用户手册，确认启动模式由 J1 拨码决定：

```text
Boot Mode   MIO[5]   MIO[4]
JTAG          0        0
QSPI          1        0
SD Card       1        1
```

Vivado 里启用 SD0 不会把启动模式改成 SD。真正影响启动模式的是 J1 上电采样，以及板子的复位和 JTAG/debug 状态。

最后按简化流程重新连接和上电后，XSCT 输出变为：

```text
1  APU
   2  ARM Cortex-A9 MPCore #0 (Running)
   3  ARM Cortex-A9 MPCore #1 (Running)
4* xc7z020
```

然后 OCM 测试通过：

```tcl
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
mwr 0x0 0x12345678
mrd 0x0
```

输出：

```text
0:   12345678
```

再运行 `Run As -> Launch on Hardware`，串口成功打印：

```text
Hello World

Successfully ran Hello World application
```

这就是这次调试的闭环点。

### 问题 5：Temp 文件 permission denied

日志中还出现过：

```text
error deleting "C:/Users/.../Temp/..." permission denied
```

这个不是本次主错误。后面 ELF 已经成功下载并运行，说明它只是临时文件清理警告，不影响 Hello World 结果。

## 当前完成了什么

这一版已经完成：

```text
[x] 使用 Vivado 导出的新 XSA 创建 accel_axi_112_uart platform
[x] platform build 成功
[x] 创建 accel_axi_112_hello application
[x] 理清 platform / application / system / domain 的关系
[x] 确认 BSP 使用 ps7_uart_1 作为 stdin/stdout
[x] 修复并避开旧 XSA 无 UART 的问题
[x] 修复 HLS driver Makefile 的 Windows build 问题
[x] 安装并确认 CP2102 USB-UART 可用
[x] 用 XSCT targets 定位 AP transaction timeout
[x] 用 OCM mwr/mrd 验证 PS debug memory access
[x] Run As -> Launch on Hardware 成功
[x] 串口看到 Hello World 输出
```

这说明：

```text
Vivado -> XSA -> Vitis platform -> application -> JTAG download -> PS UART output
```

这条最基础的上板软件链路已经跑通。

## 后续还要完成什么

下一步才是 GEMM accelerator 的板端验证：

```text
1. 新建或修改 Vitis application，不再只跑 Hello World。
2. 在 PS 端准备 A/B/C/golden/instruction buffer。
3. 把 A/B/instruction 放到 DDR。
4. 使用 Xil_DCacheFlushRange / Xil_DCacheInvalidateRange 处理 cache 一致性。
5. 通过 HLS driver 或寄存器地址配置 accelerator_top_axi。
6. 设置 instr_mem、A_mem、B_mem、C_mem、instr_num。
7. Start IP，poll done/status。
8. PS 读回 C，与 CPU golden 对比。
9. UART 打印 PASS / mismatch_count / checksum / latency。
```

后面 GEMM 应用需要重新关注 DDR。Hello World 能跑在 OCM 里，但 A/B/C 矩阵不适合放 OCM。真正的 Phase 3 成功标准是：

```text
PS 通过 AXI-Lite 控制 PL
PL 通过 m_axi_gmem + HP0 访问 DDR
GEMM 结果正确
串口打印 PASS
```

## 这一版结论

这次工作把最难受的一段 bring-up 先拆清楚了：Vivado 侧硬件图纸已经能交给 Vitis，Vitis 侧 platform/application 也能跑到板子 ARM 上，并通过 UART 打印结果。

现在还不能说加速器已经上板成功，因为还没有跑 `accelerator_top_axi` 的 GEMM 数据链路。但可以说 Phase 3 的基础运行环境已经成立。后续写 PS 端 GEMM 测试程序时，问题会集中在 AXI-Lite 寄存器配置、DDR buffer 地址、cache flush/invalidate 和 PL 计算结果校验上。
