# 从 XSA 到 Hello World：Vitis 里的 Platform、System、Application 到底是什么

这篇文章记录我第一次把 Vivado 导出的 Zynq 硬件系统接到 Vitis 软件工程时，遇到的几个最容易混淆的概念：`Platform`、`System`、`Application`，以及 application 下面 `src/` 里那几个文件到底分别负责什么。

我的环境大致是：

```text
Board: Mizar Z7 / ZYNQ-7020
Tool : Vivado/Vitis 2020.2
Flow : Vivado block design -> export XSA -> Vitis standalone application
XSA  : accel_axi_112.xsa
```

这一步还不是验证 FPGA GEMM IP 的功能，而是先把最基础的软件链路跑通：

```text
Vivado XSA
  -> Vitis Platform
  -> standalone Application
  -> ELF 下载到 ARM Cortex-A9
  -> UART 打印
  -> PS 读写 DDR
```

如果这条链路不通，后面再去调 AXI-Lite、HP DDR、HLS IP 和 GEMM 结果，很容易把问题混在一起。

## 先看一张关系图

我现在更愿意这样理解 Vitis 工程：

```text
XSA
  Vivado 导出的硬件说明包
  里面包含 PS 配置、DDR、UART、AXI IP 地址、bitstream 等信息

Platform Project
  Vitis 根据 XSA 生成的软件平台
  里面有 BSP、驱动、xparameters.h、system.mss、FSBL 等

Domain
  application 运行的软件环境
  例如 standalone on ps7_cortexa9_0

Application Project
  我真正写 main() 的地方
  例如 Hello World、DDR test、AXI-Lite test、GEMM test

System Project
  把 application、domain、platform 组织在一起的外层工程
```

放到我这个工程里，大概对应：

```text
accel_axi_112_uart                  Platform Project
accel_axi_112_hello                 Application Project
accel_axi_112_hello_system          System Project

accel_axi_112_ddr_test              Application Project
accel_axi_112_ddr_test_system       System Project
```

这里最重要的一点是：`Application` 不是孤立运行的，它依赖某个 `Platform`。如果 Vivado 硬件改了，比如 UART、DDR、AXI 地址或 HLS IP 变了，就应该重新导出 XSA，并更新或重建 platform。

## XSA 是什么

`XSA` 可以理解成 Vivado 交给 Vitis 的硬件说明文件。Vivado 负责搭硬件系统、生成 bitstream，Vitis 负责在这个硬件系统上写 ARM 端软件。

XSA 里会告诉 Vitis：

```text
PS7 怎么初始化
DDR 地址范围和初始化参数是什么
UART 走哪个 MIO
AXI-Lite 外设基地址是什么
HLS IP 有哪些寄存器和驱动
bitstream 在哪里
```

我这里使用的 XSA 是：

```text
gzy_gemm_accel/vivado_board/accel_axi_112/export/accel_axi_112.xsa
```

如果 XSA 里没有 UART，Vitis 创建 Hello World application 时可能会提示：

```text
This application requires a Uart IP in the hardware.
```

这个问题不能靠改 `helloworld.c` 解决。它说明当前 platform 对应的硬件说明里没有可用 UART，需要回 Vivado 配好 UART，重新导出 XSA，再重建或更新 platform。

## Platform Project 是什么

`Platform Project` 是 Vitis 根据 XSA 生成的软件平台。它不是业务程序，而是“这块板子的软件基础设施”。

它通常会生成：

```text
BSP
  Board Support Package，包含 standalone runtime、startup、Xilinx 驱动和库

xparameters.h
  根据 XSA 自动生成的硬件宏，例如 UART、DDR、AXI IP 的基地址

system.mss
  BSP 配置，例如 stdin/stdout 使用哪个 UART

FSBL
  First Stage Boot Loader，后续做 BOOT.BIN 或固化启动时会用到
```

所以我把 platform 理解成：

```text
Vitis 眼里的这块板子。
```

在 Project Explorer 里，`accel_axi_112_uart` 就是我的 platform。这个名字里的 `uart` 只是我为了区分旧 platform 起的名字，因为之前有一版 XSA 没有配好 UART。

## Application Project 是什么

`Application Project` 才是我写 `main()` 的地方。

例如：

```text
accel_axi_112_hello
  最基础的 Hello World，验证 JTAG 下载、ARM 运行、UART 打印

accel_axi_112_ddr_test
  DDR sanity test，验证 PS 程序跑在 DDR，并且 PS 可以读写 DDR

后续 GEMM application
  PS 准备 A/B/instr，配置 accelerator_top_axi，启动 PL，读回 C
```

第一次做 sanity test 时，我建议 template 选 `Hello World`，不要一开始选 `Empty Application`。因为 Hello World 模板会自动带好串口打印和平台初始化文件，适合一步一步排查。

创建 application 时关键选项是：

```text
Platform:
  accel_axi_112_uart

Processor:
  ps7_cortexa9_0

Domain:
  standalone on ps7_cortexa9_0

Template:
  Hello World
```

`standalone` 的意思是不跑 Linux，直接裸机运行在 ARM Cortex-A9 上。第一次上板验证时，standalone 最直接，变量也最少。

## System Project 是什么

`System Project` 是 Vitis 组织 application 的外层壳。它会把 application、domain、platform 关联起来。

比如：

```text
accel_axi_112_ddr_test_system
  -> accel_axi_112_ddr_test
     -> standalone on ps7_cortexa9_0
     -> based on accel_axi_112_uart platform
```

日常写代码时，我主要关注 application；需要运行、调试或查看组织关系时，才会注意 system。

一个容易混的点是：原来的 `accel_axi_112_hello_system` 不会自动影响新的 `accel_axi_112_ddr_test_system`。它们是不同的 system。真正要小心的是右键 Build 或 Run As 的时候不要点错 application，否则可能运行了旧的 Hello World。

## Vitis 界面里常看的窗口

Vitis 2020.2 基于 Eclipse，界面一开始会有点绕。我目前最常用的是这几个区域。

### Project Explorer

左侧工程树。这里能看到 platform、application、system。

如果看不到，可以打开：

```text
Window -> Show View -> Project Explorer
```

我通常在这里做：

```text
右键 platform -> Build Project
右键 application -> Build Project
右键 application -> Run As -> Launch on Hardware
打开 application/src/helloworld.c
打开 application/src/lscript.ld
```

### Editor

中间编辑区。打开 `helloworld.c` 时是普通 C 代码编辑器；打开 `lscript.ld` 时，Vitis 可能显示图形化的 Linker Script 页面。

`lscript.ld` 通常有两个页签：

```text
Summary
  图形界面，能看到 Memory 区域和 Section to Memory Region Mapping

Source
  真实 linker script 文本
```

如果只是检查程序放在 DDR 还是 OCM，`Summary` 页已经够用了。要批量修改或确认细节时，看 `Source` 更直接。

### Console

普通编译和运行日志窗口。Build、Launch on Hardware、下载 ELF 的信息经常在这里出现。

Build 成功时，一般能看到类似：

```text
Build Finished
```

Application build 成功后，会生成：

```text
Debug/<application_name>.elf
```

`ELF` 就是 ARM 端可执行文件。

### Problems

编译错误和警告列表。如果 build 报错，我会先看 Console 的完整日志，再看 Problems 定位是哪一行。

### Vitis Log

IDE 自己的操作日志。它更偏工具内部行为，不是每次都需要看。

### XSCT Console

`XSCT` 是 Xilinx Software Command-line Tool。它可以用 Tcl 命令连接 JTAG、选择 ARM target、读写内存、下载 ELF。

如果看到提示符：

```text
xsct%
```

说明它在等待输入命令。

常见命令有：

```tcl
connect
targets
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
mwr 0x0 0x12345678
mrd 0x0
```

它们分别表示连接 hw_server、列出 JTAG/debug target、选择 ARM Cortex-A9 核 0、暂停 CPU、写内存、读内存。

当 `targets` 能看到：

```text
APU
ARM Cortex-A9 MPCore #0
ARM Cortex-A9 MPCore #1
xc7z020
```

说明 PS 侧调试链路和 PL 侧 JTAG 至少都能被工具看到。

## src 目录里的文件分别干什么

以 `accel_axi_112_ddr_test/src` 为例，里面有：

```text
helloworld.c
platform_config.h
platform.c
platform.h
lscript.ld
Xilinx.spec
```

### helloworld.c

这是 application 的主程序，里面有 `main()`。

后续我写 DDR test、AXI-Lite register test、GEMM application，主要改的就是这个文件。

Hello World 模板最开始大概是：

```c
int main()
{
    init_platform();

    print("Hello World\n\r");
    print("Successfully ran Hello World application");

    cleanup_platform();
    return 0;
}
```

后面做 DDR sanity test 时，可以把它改成：

```c
#include "platform.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xil_cache.h"

#define DDR_TEST_ADDR 0x01000000U

int main()
{
    volatile u32 *ddr = (volatile u32 *)DDR_TEST_ADDR;
    u32 v0;
    u32 v1;

    init_platform();

    xil_printf("DDR sanity test start\r\n");
    xil_printf("DDR test addr = 0x%08x\r\n", DDR_TEST_ADDR);

    ddr[0] = 0x12345678U;
    ddr[1] = 0xDEADBEEFU;

    Xil_DCacheFlushRange((UINTPTR)ddr, 2 * sizeof(u32));
    Xil_DCacheInvalidateRange((UINTPTR)ddr, 2 * sizeof(u32));

    v0 = ddr[0];
    v1 = ddr[1];

    xil_printf("DDR[0] = 0x%08x\r\n", v0);
    xil_printf("DDR[1] = 0x%08x\r\n", v1);

    if (v0 == 0x12345678U && v1 == 0xDEADBEEFU) {
        xil_printf("DDR sanity PASS\r\n");
    } else {
        xil_printf("DDR sanity FAIL\r\n");
    }

    cleanup_platform();
    return 0;
}
```

这里的目标很小：PS 往 DDR 地址 `0x01000000` 写两个数，再读回来。如果串口打印的值一致，就说明：

```text
PS -> DDR
```

这层是通的。

### platform.c

这是模板提供的平台初始化代码。里面主要有：

```c
init_platform();
cleanup_platform();
enable_caches();
disable_caches();
init_uart();
```

对 Zynq PS UART 来说，注释里也写得很清楚：PS7 UART 通常由 BootROM/BSP 配成 115200 bps。

初学阶段我不建议随便改 `platform.c`。一般只在 `helloworld.c` 里调用：

```c
init_platform();
cleanup_platform();
```

### platform.h

`platform.c` 的头文件，声明：

```c
void init_platform();
void cleanup_platform();
```

业务代码里 `#include "platform.h"` 后，就能调用这两个函数。

### platform_config.h

平台配置宏。比如我这里有：

```c
#define STDOUT_IS_PS7_UART
#define UART_DEVICE_ID 0
```

它告诉模板代码 stdout 使用 PS7 UART。一般不用手动改。

### lscript.ld

这是 linker script，链接脚本。它决定程序的各个段放到哪个内存区域。

这个文件非常关键，因为它决定：

```text
.text   代码段放哪里
.rodata 只读数据放哪里
.data   已初始化全局变量放哪里
.bss    未初始化全局变量放哪里
.heap   malloc/new 使用的堆放哪里
.stack  函数调用栈放哪里
```

Zynq 工程里常见 memory 区域有：

```ld
MEMORY
{
   ps7_ddr_0 : ORIGIN = 0x100000, LENGTH = 0x3FF00000
   ps7_ram_0 : ORIGIN = 0x0, LENGTH = 0x30000
   ps7_ram_1 : ORIGIN = 0xFFFF0000, LENGTH = 0xFE00
}
```

其中：

```text
ps7_ddr_0
  外部 DDR，容量大，后续 GEMM 的 A/B/C buffer 主要会在这里。

ps7_ram_0
  PS OCM，On-Chip Memory，片上 RAM，地址 0x0 附近，容量小。

ps7_ram_1
  另一段片上 RAM 区域，地址接近 0xFFFF0000。
```

如果 `lscript.ld` 里看到：

```ld
} > ps7_ddr_0
```

说明对应 section 放在 DDR。

如果看到：

```ld
} > ps7_ram_0
```

说明对应 section 放在 OCM。

所以判断程序到底跑在哪里，不要靠猜，直接看 `lscript.ld` 或图形界面的 `Section to Memory Region Mapping`。

### Xilinx.spec

这是 Xilinx 工具链使用的 spec 文件，影响底层编译/链接启动文件选择。Hello World 模板里通常很小，例如：

```text
*startfile:
crti%O%s crtbegin%O%s
```

初学阶段不用碰它。

## 程序默认放 DDR 吗

在 Zynq standalone application 里，如果 XSA 里 DDR 配置正常，Vitis 生成 application 时通常会默认把主要 section 放到 `ps7_ddr_0`。

但是这不是一条永远成立的规则。最终以 linker script 为准：

```text
.text/.data/.bss/.heap/.stack -> ps7_ddr_0
  程序主要跑在 DDR

.text/.data/.bss/.heap/.stack -> ps7_ram_0
  程序主要跑在 OCM
```

有时候为了排查 DDR 是否有问题，可以故意把 Hello World 放到 OCM。这样能把问题拆开：

```text
Hello World 放 OCM 也跑不起来:
  先查 JTAG、PS debug、boot mode、reset。

Hello World 放 OCM 能跑，但放 DDR 不行:
  再查 DDR 初始化、linker script、ps7_init、硬件 DDR 配置。
```

我之前能在 OCM 附近跑 Hello World，说明 JTAG 下载、ARM 运行和 UART 输出基本通了。下一步再把程序放回 DDR，并加 DDR 读写测试，就是为了验证 `PS -> DDR` 这一层。

## Build、Clean、Launch 分别是什么

### Build Project

`Build Project` 是编译当前工程。

对 platform 来说，它会编译 BSP、驱动和库。

对 application 来说，它会把：

```text
helloworld.c
platform.c
startup code
BSP library
lscript.ld
```

编译链接成：

```text
Debug/<application_name>.elf
```

如果只是改了 `helloworld.c`，通常直接 Build 就够了。

### Clean Project

`Clean Project` 是清掉旧的编译产物，例如：

```text
Debug/*.o
Debug/*.elf
```

它不是删除源码，也不会删除 application。

什么时候建议 Clean？

```text
刚改了 helloworld.c:
  可以直接 Build

刚改了 lscript.ld:
  建议 Clean 后再 Build

刚更新了 platform / BSP / XSA:
  建议 Clean 后再 Build
```

Clean 主要是防止 Vitis/Eclipse 没有正确发现生成配置或 linker script 的变化。

### Run As -> Launch on Hardware

右键 application：

```text
Run As -> Launch on Hardware
```

它不是简单运行一个 Windows exe，而是通过 JTAG 把程序放到板上的 ARM 里运行。

通常会做这些事：

```text
连接 hw_server
通过 JTAG 连接板子
下载 bitstream 到 PL
运行 ps7_init.tcl 初始化 PS
下载 application ELF 到 ARM Cortex-A9
设置 PC 到程序入口
启动 ARM 运行
```

Launch 成功时，Console 里常见类似：

```text
Downloading Program -- .../Debug/accel_axi_112_ddr_test.elf
Setting PC to Program Start Address ...
Successfully downloaded ...
Info: ARM Cortex-A9 MPCore #0 Running
```

同时串口工具应该能看到程序打印。

## 为什么第一层先做 DDR sanity test

我的最终目标是让 PS 控制 PL 里的 `accelerator_top_axi`，并让 PL 通过 HP port 访问 DDR 中的 A/B/C/instr buffer。

完整链路其实是：

```text
PS
  -> 写 DDR 中的 A/B/instr
  -> 通过 AXI-Lite 配置 PL IP
  -> PL 通过 HP0 读 DDR
  -> PL 计算 GEMM
  -> PL 写 DDR 中的 C
  -> PS 读回 C 并比较 golden
```

如果一开始就跑完整 GEMM，出错时可能不知道问题在：

```text
PS 程序有没有跑起来
DDR 初始化是否正常
PS 自己能不能读写 DDR
AXI-Lite 控制寄存器是否能访问
PL 是否真的启动
HP0 是否能访问 DDR
GEMM 算法是否正确
cache 是否需要 flush/invalidate
```

所以我把上板流程拆成三层：

```text
第 1 层：PS-DDR sanity test
  只验证 PS 程序跑在 DDR，并且 PS 能读写 DDR。

第 2 层：AXI-Lite / IP register sanity test
  验证 PS 能访问 accelerator_top_axi 的控制寄存器。

第 3 层：PS-PL-DDR GEMM application
  PS 准备数据，启动 PL，读回 C，和 CPU golden 对比。
```

这样每一层都只解决一个问题，后面调试会清楚很多。

## 第 1 层 DDR test 的实际流程

### 1. 新建 application

```text
File -> New -> Application Project
Platform: accel_axi_112_uart
Application project name: accel_axi_112_ddr_test
System project: Create new
Processor: ps7_cortexa9_0
Domain: standalone on ps7_cortexa9_0
Template: Hello World
Finish
```

### 2. 检查 linker script

打开：

```text
accel_axi_112_ddr_test
  -> src
     -> lscript.ld
```

在 Summary 页看：

```text
Section to Memory Region Mapping
```

确认 `.text/.data/.bss/.heap/.stack` 等 section 都是：

```text
ps7_ddr_0
```

如果打开 Source 页，也可以直接看是不是：

```ld
} > ps7_ddr_0
```

### 3. 修改 helloworld.c

把 Hello World 改成 DDR 写读测试。核心逻辑是：

```c
volatile u32 *ddr = (volatile u32 *)0x01000000U;

ddr[0] = 0x12345678U;
ddr[1] = 0xDEADBEEFU;

Xil_DCacheFlushRange((UINTPTR)ddr, 2 * sizeof(u32));
Xil_DCacheInvalidateRange((UINTPTR)ddr, 2 * sizeof(u32));

v0 = ddr[0];
v1 = ddr[1];
```

这里用 `volatile` 是为了告诉编译器：这个地址对应真实内存访问，不要随便优化掉读写。

这里用 cache flush/invalidate，是因为 ARM 端开 cache 后，CPU 写的数据可能先停在 cache 里。虽然这个简单 PS 自读测试不一定每次都暴露 cache 问题，但后面 PS 和 PL 共享 DDR 时，cache 一定要认真处理。

### 4. Build

右键：

```text
accel_axi_112_ddr_test -> Build Project
```

如果只是改了 `helloworld.c`，可以直接 Build。  
如果刚动过 `lscript.ld` 或 platform，建议先 Clean 再 Build。

成功后应该生成：

```text
accel_axi_112_ddr_test/Debug/accel_axi_112_ddr_test.elf
```

### 5. Launch on Hardware

右键：

```text
accel_axi_112_ddr_test -> Run As -> Launch on Hardware
```

注意不要点到旧的 `accel_axi_112_hello`。

串口工具设置：

```text
115200 baud
8 data bits
no parity
1 stop bit
no flow control
```

如果成功，串口应该看到类似：

```text
DDR sanity test start
DDR test addr = 0x01000000
DDR[0] = 0x12345678
DDR[1] = 0xdeadbeef
DDR sanity PASS
```

这说明第 1 层：

```text
PS -> DDR
```

已经通过。

## 常见错误和排查方向

### 点错工程

如果串口还是打印：

```text
Hello World
Successfully ran Hello World application
```

而不是 DDR test，通常是运行了旧的 `accel_axi_112_hello`。检查 Run As 的 application 是否是 `accel_axi_112_ddr_test`。

### Linker script 还是 OCM

如果 `lscript.ld` 里 `.text` 等 section 还是：

```ld
} > ps7_ram_0
```

那程序主要放在 OCM，不是 DDR。打开 Summary 页，把 section 映射到 `ps7_ddr_0`，保存后建议 Clean 再 Build。

### Build 成功但没有新 ELF

检查是不是右键点到了 system 或 platform。应用 ELF 通常在 application 的 `Debug/` 目录下。

### Launch 报 AP transaction timeout

这类问题通常不是 `helloworld.c` 的逻辑错误，而是 JTAG/PS debug/boot mode/reset 状态不正常。

可以在 XSCT Console 里先看：

```tcl
connect
targets
```

如果只能看到 `xc7z020`，但 APU / ARM Cortex-A9 状态异常，就先排查板卡上电、JTAG 线、boot mode、reset 和 hw_server。

### 串口没输出

先确认：

```text
板子的 UART USB 线是否连接
Windows 设备管理器里是否有 CP210x COM 口
串口工具是否选对 COM
波特率是否 115200
platform 的 stdin/stdout 是否是 ps7_uart_1
```

如果 Hello World 能输出，但 DDR test 不能输出，说明 UART 大概率没问题，再看程序是否真的被下载运行。

## 我目前形成的判断习惯

遇到 Vitis 工程时，我现在先问这几个问题：

```text
1. 当前 application 依赖的是哪个 platform？
2. 这个 platform 来自哪个 XSA？
3. 右键 Build / Run 的是不是目标 application？
4. lscript.ld 里程序段到底放 DDR 还是 OCM？
5. Build 后生成的 ELF 是哪一个？
6. Launch 时下载的是不是这个 ELF？
7. 串口输出是否对应当前代码？
```

这几个问题看似琐碎，但能避免很多“代码明明改了，板子却像没变”的情况。

## 小结

我现在对 Vitis 这几个概念的理解是：

```text
XSA:
  Vivado 导出的硬件交接文件。

Platform:
  Vitis 根据 XSA 生成的板级软件基础。

Domain:
  application 运行的软件环境，比如 standalone on ps7_cortexa9_0。

Application:
  我写 main() 的地方，最终生成 ELF。

System:
  把 application、domain、platform 组织起来的外层工程。

lscript.ld:
  决定程序段放 DDR 还是 OCM 的关键文件。
```

第一次上板不要急着验证完整算法。先让 Hello World 通，再让 DDR sanity test 通，再测 AXI-Lite 寄存器，最后再跑 PS-PL-DDR 的 GEMM application。这样每一步失败时，都能知道自己正在排查哪一层。
