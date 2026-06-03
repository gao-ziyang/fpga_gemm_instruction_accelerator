# Vitis/XSCT 调试记录：AP transaction timeout、DDR mwr/mrd 验证，以及串口没输出的原因

这篇记录一个 Zynq 裸机程序下载时遇到的典型问题：

```text
Error while launching program:
AP transaction timeout
```

现象是 Vitis 直接 `Run As -> Launch on Hardware` 下载 ELF 失败。有时先给 Zynq 断电重启、再 `Program Device`、最后重新 `Run As` 可以恢复，但后续复现实验说明这不是充分条件。`Program Device` 成功只说明 PL bitstream 能下载，不说明 PS 侧 DAP/APU 一定能被调试。

后面做 AXI-Lite/IP register sanity test 时，又观察到一个很有代表性的现象：刚开始 `targets` 能正常看到 `APU` 和两个 `ARM Cortex-A9`，但一执行 GUI `Run As`，ELF 下载失败，随后 `targets` 变成 DAP/AP transaction error。不过后续复测里，手动 XSCT 连续下载 5 次成功，GUI `Run As` 也连续成功 2 次。因此现在不能简单定性为 “Run As 一定有问题”。更准确的结论是：关键在于当前 PS DAP/A9 debug 状态是否干净；状态干净时，手动 XSCT 和 GUI Run As 都能成功，状态坏掉时两者都可能失败。

这类问题很容易被误判成 C 代码、linker script 或 DDR test 代码错误。实际上，关键要先看 XSCT 里的 target 状态。

## 1. AP transaction timeout 是什么状态

失败时，XSCT 里执行：

```tcl
connect
targets
```

看到：

```text
1  APU (Cannot open JTAG port: AP transaction error, DAP status 0xF0000021)
   2* ARM Cortex-A9 MPCore #0 (APB Memory access port is disabled)
   3  ARM Cortex-A9 MPCore #1 (APB Memory access port is disabled)
4  xc7z020
```

这说明：

```text
PL 侧 JTAG 能看到 xc7z020
PS 侧 APU 没有正常打开
ARM Cortex-A9 debug port 不可访问
```

所以这时不要急着改 application 代码，也不要反复 Build。ELF 还没有真正跑起来，问题在 JTAG/hw_server/PS debug access 这一层。

## 2. 这次有效的恢复顺序

这次直接 `Launch on Hardware` 不稳定。曾经成功过的顺序是：

```text
1. Zynq 板子断电重启。
2. 在 Vitis/Xilinx 工具里先 Program Device。
3. 再 Run As -> Launch on Hardware。
4. 如果要看串口输出，串口上位机要在 application 运行前打开。
```

但后面又遇到：

```text
Cannot reset APU. AP transaction error, DAP status 0xF0000021
```

这说明关键不在这个顺序本身，而在 `Run As` 过程中 PS DAP/APU 是否真的可访问。

Vitis 生成的调试 Tcl 大致包含：

```tcl
targets -set -nocase -filter {name =~"APU*"}
rst -system
after 3000
fpga -file ...
source .../ps7_init.tcl
ps7_init
ps7_post_config
targets -set -nocase -filter {name =~ "*A9*#0"}
dow .../app.elf
con
```

其中 `Cannot reset APU` 就是卡在 APU reset/debug 这层。只要 APU/DAP 不可访问，后面的 `ps7_init`、`dow`、`con` 都不可靠。

更可靠的判定标准是：`targets` 不能有 DAP/AP transaction error；选择 A9 后 `stop` 必须成功。只有 `stop` 成功，才继续 `ps7_init`、`dow`、`con`。

后续 AXI-Lite/IP register sanity test 里，我又看到：

```text
connect
targets
  1  APU
     2  ARM Cortex-A9 MPCore #0 (Running)
     3  ARM Cortex-A9 MPCore #1 (Running)
  4* xc7z020

Run As -> Failed to download accel_axi_112_ip_reg_test.elf

targets
  1* APU (Cannot open JTAG port: AP transaction error, DAP status 0xF0000021)
     2  ARM Cortex-A9 MPCore #0 (APB Memory access port is disabled)
     3  ARM Cortex-A9 MPCore #1 (APB Memory access port is disabled)
  4  xc7z020
```

这个现象说明：那一次不是 application 运行后访问 AXI-Lite 卡死，而是 ELF 还没下载进去，PS debug 链路已经坏了。当时最好的处理不是继续点 `Run As`，而是重新恢复到 `targets` 正常、A9 `stop` 成功的状态，再手动下载 ELF。

但为了避免误判，我又补做了重复实验：

```text
手动 XSCT repeat 脚本连续下载运行 5 次：全部 Successfully downloaded。
脚本结束后 targets 仍能看到 APU / A9 / xc7z020。
随后 GUI Run As -> Launch on Hardware 连续 2 次：也都 Successfully downloaded。
```

所以最新判断是：`Run As` 曾经在坏状态附近失败过，但还不能证明它本身必然会把 DAP/APU 打坏。这个问题更像是 PS DAP/JTAG debug 链路偶发不稳定，GUI 和手动流程只是两种触发/暴露方式。

成功后，Vitis Console 里能看到类似：

```text
Downloading Program -- C:/.../accel_axi_112_ddr_test.elf
    section, .text: 0x00100000 - 0x001020cb
    section, .rodata: 0x001020e4 - 0x00102283
    section, .data: 0x00102288 - 0x001026f7
    section, .bss: 0x00108008 - 0x0010802f
    section, .heap: 0x00108030 - 0x0010a02f
    section, .stack: 0x0010a030 - 0x0010d82f

Setting PC to Program Start Address 0x00100000
Successfully downloaded ...
Info: ARM Cortex-A9 MPCore #0 Running
```

这些 section 地址都在 `0x00100000` 附近，说明 ELF 的代码、数据、堆栈已经放到了 DDR 区域，而不是 OCM 的 `0x0` 附近。

## 3. Run As 到底做了什么

`Run As -> Launch on Hardware` 不是在电脑上运行一个 `.exe`，而是让 Vitis 通过 JTAG 接管板子上的 ARM，然后把程序下载进去运行。

它大概做这些事：

```text
1. 连接 hw_server。
2. 扫描 JTAG 链，找到 Zynq。
3. 找到 PL target，也就是 xc7z020。
4. 找到 PS debug target，也就是 DAP/APU/ARM Cortex-A9。
5. 根据 launch 配置 reset system 或 reset APU。
6. 下载 bitstream 到 PL。
7. 执行 ps7_init.tcl，初始化 PS、DDR、UART、时钟等。
8. 把 ELF 下载到 OCM 或 DDR。
9. 设置 ARM 的 PC 到 ELF 入口地址。
10. 让 ARM Cortex-A9 #0 开始运行。
```

所以它至少依赖两条链路：

```text
PL 编程链路:
  PC -> USB/JTAG -> xc7z020
  用于 Program Device / 下载 bitstream。

PS 调试链路:
  PC -> USB/JTAG -> DAP -> APU -> ARM Cortex-A9
  用于 reset、stop、dow ELF、con。
```

这就解释了一个很反直觉的现象：

```text
Program Device 成功，不代表 Run As 一定成功。
```

因为 `Program Device` 主要证明 PL 侧 `xc7z020` 可以被 JTAG 编程；而 `Run As` 还必须能控制 PS 里的 DAP/APU/ARM。

这几个报错可以对应到不同阶段：

```text
AP transaction timeout
  Vitis 想通过 DAP 访问 PS/ARM，但等不到正常响应。

Cannot reset APU. AP transaction error
  Run As 正在尝试 reset APU，但 DAP/APU 通道失败。

Failed to download app.elf
  前面某些步骤可能过了，但下载 ELF 到 ARM 可访问内存时失败。

whole scan chain (DR shift output all ones)
  更底层的 JTAG 扫描链状态异常，说明当前 debug 链路不干净。
```

所以遇到这些错误时，优先不要怀疑：

```text
C 代码
linker script
DDR test 地址
串口 printf
```

更应该先确认：

```text
DAP/APU/A9 是否真的可控。
```

## 4. DAP、APU、A9、xc7z020 分别是什么

Zynq 可以粗略分成两部分：

```text
PL:
  FPGA 逻辑部分。

PS:
  ARM 处理器系统部分。
```

XSCT 里的几个名字可以这样理解：

```text
xc7z020
  Zynq 芯片里的 FPGA/PL 侧 target。
  能看到它，说明 PL 侧 JTAG 扫描基本可见。

DAP
  Debug Access Port。
  它是通过 JTAG 进入 PS/ARM 调试系统的门。

APU
  Application Processor Unit。
  可以理解成 ARM Cortex-A9 处理器子系统。

ARM Cortex-A9 MPCore #0
  第 0 个 ARM 核。
  裸机 application 通常运行在 ps7_cortexa9_0，也就是这个核上。

ARM Cortex-A9 MPCore #1
  第 1 个 ARM 核。
  这次 DDR sanity test 不需要操作它。
```

链路可以画成：

```text
PC
  -> USB/JTAG
    -> Zynq JTAG
      -> PL: xc7z020
      -> PS debug: DAP
           -> APU
              -> ARM Cortex-A9 #0 / #1
```

所以坏状态经常是：

```text
DAP (Cannot open JTAG port: AP transaction error...)
xc7z020
```

这表示：

```text
PL 看得见，但通向 ARM 的调试入口打不开。
```

好状态至少应该是：

```text
APU
  ARM Cortex-A9 MPCore #0
  ARM Cortex-A9 MPCore #1
xc7z020
```

但注意：看到 APU/A9 还不够。真正的硬验证是能不能 `stop` A9 #0。

## 5. whole scan chain 是什么，stop 又是什么

有时 `targets` 会出现：

```text
whole scan chain (DR shift output all ones)
```

这句话可以先理解成：

```text
JTAG 扫描链读出来一串全 1，工具认为链路里有异常状态。
```

它不一定代表板子坏了，但它说明当前 JTAG/debug 会话不干净。后面如果再出现：

```text
stop
AP transaction timeout
```

就说明虽然 XSCT 列出了 APU/A9，但实际控制 ARM 的通道仍然不稳定。

`stop` 不是关闭 ARM 核，也不是断电。它只是暂停当前选中的调试目标。

例如：

```tcl
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
```

这两句的意思是：

```text
先选中 ARM Cortex-A9 #0。
然后暂停这个 ARM 核。
```

它不会暂停两个核，只会作用到当前选中的 target。  
如果要暂停 #1，需要另外选：

```tcl
targets -set -nocase -filter {name =~ "*A9*#1"}
stop
```

但通常裸机程序跑在 #0，所以我们只操作 #0。

判断标准是：

```text
只看到 xc7z020:
  只能说明 PL 侧可见。

看到 APU/A9:
  说明工具列出了 PS 侧目标，但不保证可控。

A9 #0 stop 成功:
  才说明 PS debug 链路真的可用。

dow/con 成功:
  ELF 才真正下载并运行。
```

所以 `stop` 是 `Run As` 之前很有价值的体检。如果 `stop` 都失败，`Run As` 大概率也会失败。

## 6. XSCT 基本命令

下面是这次用到的最小 XSCT 命令集。

### connect

```tcl
connect
```

连接本机的 `hw_server`。如果没有运行，Vitis/XSCT 会尝试启动：

```text
attempting to launch hw_server
```

成功后会返回类似：

```text
tcfchan#1
```

### targets

```tcl
targets
```

列出当前 JTAG/debug 能看到的目标。

坏状态：

```text
APU (Cannot open JTAG port...)
ARM Cortex-A9 ... (APB Memory access port is disabled)
xc7z020
```

好状态：

```text
1  APU
   2* ARM Cortex-A9 MPCore #0 (Running)
   3  ARM Cortex-A9 MPCore #1 (Running)
4  xc7z020
```

### targets -set

```tcl
targets -set -nocase -filter {name =~ "*A9*#0"}
```

选择 ARM Cortex-A9 核 0 作为当前操作对象。后面的 `stop`、`mwr`、`mrd`、`dow`、`con` 都会针对当前选中的 target。

### stop

```tcl
stop
```

暂停当前 CPU。暂停后更适合读写内存、重新下载 ELF 或观察 PC 停在哪里。

例如程序已经跑完时，可能看到：

```text
Stopped at 0x1020c8
_exit() at _exit.c: 16
```

这说明 application 已经执行结束，CPU 停在 `_exit` 附近。

### dow

```tcl
dow C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ddr_test/Debug/accel_axi_112_ddr_test.elf
```

`dow` 是 download 的意思，把 ELF 下载到当前选中的处理器对应内存里，并设置程序入口。

如果已经在 Vitis GUI 里 `Run As -> Launch on Hardware` 成功过，一般不需要手动 `dow`。但如果串口没开、错过输出，可以打开串口后重新：

```tcl
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
dow C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ddr_test/Debug/accel_axi_112_ddr_test.elf
con
```

### con

```tcl
con
```

continue 的意思，让当前 CPU 从当前 PC 继续运行。

如果 CPU 已经停在 `_exit`，单独 `con` 一般不会重新打印串口信息，因为程序已经结束了。要重新看串口输出，应该先重新 `dow` ELF，再 `con`。

## 7. 一次输入多条 XSCT 命令的方法

如果 XSCT Console 里一次只能输入一句命令，最推荐的方法不是反复复制很多行，而是把命令保存成 `.tcl` 脚本，然后在 XSCT 里只输入一句：

```tcl
source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_ip_reg_test.tcl
```

这个脚本内容大致是：

```tcl
connect
targets
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
configparams force-mem-access 1
source C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/_ide/psinit/ps7_init.tcl
ps7_init
ps7_post_config
dow C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/Debug/accel_axi_112_ip_reg_test.elf
configparams force-mem-access 0
con
```

其中 `source` 的意思是让 XSCT 读取并执行一个 Tcl 文件。这样以后不用一行一行敲，也能减少路径输错的概率。

另一个办法是把命令用分号接成一行：

```tcl
connect; targets; targets -set -nocase -filter {name =~ "*A9*#0"}; stop
```

但这种方式可读性差，而且一旦中间某一步失败，后面命令可能还会继续执行。对于上板调试，`.tcl` 脚本更稳。

## 8. mwr 和 mrd 是什么

### mwr：memory write

```tcl
mwr 0x01000000 0x12345678
```

意思是往地址 `0x01000000` 写入 32-bit 数据 `0x12345678`。

再比如：

```tcl
mwr 0x01000004 0xdeadbeef
```

意思是往下一个 32-bit word 地址写入 `0xDEADBEEF`。

地址为什么加 4？

因为一次写的是 32-bit，也就是 4 字节：

```text
0x01000000: 第 0 个 32-bit word
0x01000004: 第 1 个 32-bit word
```

### mrd：memory read

```tcl
mrd 0x01000000
```

意思是从地址 `0x01000000` 读一个 32-bit word。

如果刚才写成功，会看到：

```text
1000000:   12345678
```

再读：

```tcl
mrd 0x01000004
```

如果成功，会看到：

```text
1000004:   DEADBEEF
```

这一组命令证明：

```text
XSCT -> ARM debug port -> PS memory system -> DDR
```

这条路径是通的。

## 9. OCM 和 DDR 都可以这样测

先测 OCM：

```tcl
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
mwr 0x0 0x12345678
mrd 0x0
```

读回：

```text
0:   12345678
```

说明 PS debug 口能访问片上 OCM。

再测 DDR：

```tcl
mwr 0x01000000 0x12345678
mwr 0x01000004 0xdeadbeef
mrd 0x01000000
mrd 0x01000004
```

读回：

```text
1000000:   12345678
1000004:   DEADBEEF
```

说明 DDR 读写也通。

如果 OCM 都不能读写，优先查 JTAG、PS debug、板卡供电和 reset 状态。  
如果 OCM 能读写但 DDR 不能读写，再查 DDR 初始化、linker script、PS7 配置。

## 10. 为什么 DDR 读写成功，但串口没有输出

这次 Console 里出现了：

```text
Info: ARM Cortex-A9 MPCore #0 Running
...
Stopped at 0x1020c8
_exit() at _exit.c: 16
```

这说明 application 已经运行过，并且已经结束。

如果串口上位机还没打开，或者打开时程序已经跑完，那么程序打印的几行 UART 信息已经发出去了，但是电脑没有接收。UART 不是日志缓冲系统，后面再打开串口工具，不能追回之前已经发出的字符。

所以 DDR 能用 XSCT `mwr/mrd` 读写成功，但串口没有输出，常见原因是：

```text
1. 串口工具是在程序跑完后才打开的。
2. 串口工具选错 COM 口。
3. 波特率不是 115200 8N1。
4. 程序已经停在 _exit，单独 con 不会重新执行 main。
```

解决方法是：

```text
1. 先 Program Device，让板端 bitstream 状态稳定。
2. 打开串口工具，选对 COM，设置 115200 8N1，无流控。
3. 重新下载并运行 ELF。
```

GUI 方式：

```text
右键 application
  -> Run As
  -> Launch on Hardware
```

XSCT 方式：

```tcl
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
dow C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ddr_test/Debug/accel_axi_112_ddr_test.elf
con
```

如果串口窗口已经提前打开，这时应该能看到 application 打印。

## 11. AXI-Lite/IP register sanity test 的成功路径

第二层测试的目标是验证：

```text
PS -> AXI-Lite -> accelerator_top_axi control register
```

这一步不启动 GEMM 计算，只读写 HLS IP 自动生成的控制寄存器。当前 IP 的 AXI-Lite 基地址在 `xparameters.h` 中是：

```c
#define XPAR_ACCELERATOR_TOP_AXI_0_S_AXI_CONTROL_BASEADDR 0x40000000
```

HLS driver 生成的寄存器偏移包括：

```text
0x00  AP_CTRL
0x18  instr_mem low 32-bit
0x1c  instr_mem high 32-bit
0x24  A_mem low 32-bit
0x28  A_mem high 32-bit
0x30  B_mem low 32-bit
0x34  B_mem high 32-bit
0x3c  C_mem low 32-bit
0x40  C_mem high 32-bit
0x48  instr_num
```

Vitis 中新建 application：

```text
accel_axi_112_ip_reg_test
```

程序只做：

```text
1. 读取 AP_CTRL，打印 ap_start/ap_done/ap_idle/ap_ready。
2. 写 instr_mem/A_mem/B_mem/C_mem 这些 64-bit 地址寄存器。
3. 写 instr_num。
4. 再读回来比较。
5. 串口打印 AXI-Lite register sanity PASS/FAIL。
```

这一步必须先 `Program Device`，因为 `0x40000000` 后面的 AXI-Lite 从设备在 PL 里。如果 PL 没有加载 bitstream，PS 去访问这个地址就不是在访问正确的 IP。

有一次 GUI `Run As` 在 ELF 下载阶段失败，所以当时改用 XSCT 手动流程：

```tcl
connect
targets
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
configparams force-mem-access 1
source C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/_ide/psinit/ps7_init.tcl
ps7_init
ps7_post_config
dow C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/Debug/accel_axi_112_ip_reg_test.elf
configparams force-mem-access 0
con
```

成功时 `dow` 会显示 ELF 下载到 DDR：

```text
Downloading Program -- C:/Transformer/.../accel_axi_112_ip_reg_test.elf
section, .text: 0x00100000 - ...
section, .data: 0x00101948 - ...
section, .bss: 0x00108008 - ...
Setting PC to Program Start Address 0x00100000
Successfully downloaded ...
Info: ARM Cortex-A9 MPCore #0 Running
```

后续又用 repeat 脚本连续跑了 5 次手动下载运行，全部成功；之后 GUI `Run As -> Launch on Hardware` 又连续成功 2 次。这个结果说明：第二层本身已经稳定通过；至于 DAP/AP transaction timeout，现在不能只归因于 GUI Run As，仍应归类为 JTAG/DAP/PS debug 状态偶发不稳。

串口输出正确后，说明第二层通过。这个结论很具体：PS 已经能通过 AXI-Lite 读写 `accelerator_top_axi` 的控制寄存器，但还没有证明 PL 能通过 AXI master/HP 口访问 DDR，也还没有证明 GEMM 结果正确。第三层才会验证完整 PS-PL-DDR GEMM 数据链路。

## 12. 推荐的最小调试顺序

以后遇到类似问题，我按这个顺序来：

```text
1. Zynq 断电重启。
2. Program Device。
3. XSCT:
   connect
   targets
4. 确认 targets 没有 DAP/AP transaction error，且 APU / ARM Cortex-A9 正常出现。
5. 选择 A9 并 stop，stop 成功才继续：
   targets -set -nocase -filter {name =~ "*A9*#0"}
   stop
6. XSCT 测 OCM:
   mwr 0x0 0x12345678
   mrd 0x0
7. 如果要手动 dow 到 DDR，先初始化 PS/DDR：
   source C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ddr_test/_ide/psinit/ps7_init.tcl
   ps7_init
   ps7_post_config
8. XSCT 测 DDR:
   mwr 0x01000000 0x12345678
   mrd 0x01000000
9. 提前打开串口工具。
10. 如果体检通过，可以先用 XSCT dow/con 做稳定性确认；GUI Run As 也可以试，但一旦失败要立刻回到 targets/stop 判断 DAP 状态。
11. 如果 XSCT Console 不方便一行行输入，就用：
    source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_ip_reg_test.tcl
```

这样可以把问题分层：

```text
targets 不正常，或者 A9 stop 失败:
  JTAG / hw_server / PS debug 问题

OCM mwr/mrd 不正常:
  PS debug 或启动状态问题

DDR mwr/mrd 不正常:
  DDR 初始化或地址映射问题

DDR 正常但串口没输出:
  多半是 UART 连接时机、COM 口、波特率，或者程序已经运行结束
```

## 13. 可以如何补救

现在更准确的补救思路是：先不要追着 `Run As` 点，也不要一开始怀疑 C 代码。先把 PS debug 链路恢复到可控状态。

### 第一层：清掉软件侧占用

先保证只有一个 Xilinx 工具在连板子。

```text
1. 关闭 Vitis。
2. 关闭 Vivado Hardware Manager。
3. 关闭串口工具不一定必须，但先关掉能减少干扰判断。
4. Windows 任务管理器里结束残留：
   hw_server.exe
   xsct.exe
   xsdb.exe
```

如果 Vivado 和 Vitis 同时抢同一个 hw_server / JTAG cable，target 状态会更混乱。

### 第二层：板子重新进入干净状态

```text
1. Zynq 板子断电。
2. 等 10 秒。
3. 重新上电。
4. 先不要 Run As。
5. 先用 XSCT connect / targets 看状态。
```

这一步的目标不是马上跑程序，而是先看：

```text
targets 有没有 DAP/AP transaction error。
A9 #0 能不能 stop。
```

### 第三层：降低 JTAG 频率

如果找不到 GUI 里的 JTAG 速度设置，可以用 Vivado Tcl Console 尝试：

```tcl
open_hw_manager
connect_hw_server
open_hw_target
set_property PARAM.FREQUENCY 3000000 [current_hw_target]
get_property PARAM.FREQUENCY [current_hw_target]
```

如果 3 MHz 还不稳，可以试：

```tcl
set_property PARAM.FREQUENCY 1000000 [current_hw_target]
```

JTAG 频率太高、线材质量一般、USB 口供电/连接不稳时，降低频率有时能明显改善 DAP/AP transaction error。

### 第四层：验证 PS debug，而不是直接 Run As

```tcl
connect
targets
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
```

只有 `stop` 成功，才继续：

```tcl
configparams force-mem-access 1
source C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ddr_test/_ide/psinit/ps7_init.tcl
ps7_init
ps7_post_config
dow C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ddr_test/Debug/accel_axi_112_ddr_test.elf
configparams force-mem-access 0
con
```

如果 `stop` 失败，就不要继续 `dow`，因为这说明 PS debug 链路本身还不可控。

### 第五层：硬件侧可能原因

这有没有可能是线不稳定？有可能，而且从现象看是一个合理怀疑，但不能只归因于线。

可能原因包括：

```text
JTAG Micro USB 线质量不好，或者只是供电线不是数据线。
USB 口接触不稳。
用了 USB hub，信号或供电不稳。
板子供电不足或复位状态异常。
hw_server / xsct 残留会话没有清干净。
Vivado 和 Vitis 同时连接同一块板。
JTAG 频率偏高。
板上 PS debug/DAP 状态被上一次失败的 reset/launch 搞乱。
```

我会优先按这个顺序试：

```text
1. 换一根确认能传数据的短 Micro USB 线。
2. 换电脑 USB 口，优先直连，不走 hub。
3. 降 JTAG frequency 到 3 MHz 或 1 MHz。
4. 保证同一时间只开一个 Xilinx 硬件连接工具。
5. 每次失败后先 disconnect，再必要时结束 hw_server，最后再断电重试。
```

如果换线、换口、降频后 `targets` 仍长期只显示：

```text
DAP (Cannot open JTAG port: AP transaction error ...)
xc7z020
```

那问题就更偏向板卡供电、reset、JTAG/DAP 硬件状态或板卡本身，而不是 Vitis application。

## 可以直接发到社区的问题

下面这段可以直接发到中文 FPGA/Xilinx 社区里，语气保持成真实排查记录：

```text
大家好，我在用 Vitis 2020.2 调试 Zynq-7020 裸机程序时遇到一个比较反复的问题，想请教一下。

我的流程是 Vivado 导出 XSA，在 Vitis 里建 standalone platform 和 application。PL 里有一个 HLS 导出的 accelerator_top_axi，AXI-Lite control 基地址是 0x40000000。现在第一层 PS-DDR sanity test 已经通过，第二层 AXI-Lite/IP register sanity test 也可以通过手动 XSCT 和 GUI Run As 跑通。

稳定成功的手动 XSCT 流程是：

connect
targets
targets -set -nocase -filter {name =~ "*A9*#0"}
stop
configparams force-mem-access 1
source C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/_ide/psinit/ps7_init.tcl
ps7_init
ps7_post_config
dow C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/Debug/accel_axi_112_ip_reg_test.elf
configparams force-mem-access 0
con

这样能成功下载 ELF，串口也能打印 AXI-Lite register sanity PASS。我还做过一次手动 XSCT repeat 脚本连续 5 次下载运行，5 次都 Successfully downloaded。随后又试了 Vitis GUI Run As -> Launch on Hardware 两轮，也都成功。

但我遇到的问题是：有时候刚 connect 后 targets 是正常的：

1  APU
   2  ARM Cortex-A9 MPCore #0 (Running)
   3  ARM Cortex-A9 MPCore #1 (Running)
4* xc7z020

这时如果我点 Vitis GUI 的 Run As -> Launch on Hardware，ELF 下载会失败：

Failed to download C:/Transformer/.../accel_axi_112_ip_reg_test.elf

之后再看 targets，就变成：

1* APU (Cannot open JTAG port: AP transaction error, DAP status 0xF0000021)
   2  ARM Cortex-A9 MPCore #0 (APB Memory access port is disabled)
   3  ARM Cortex-A9 MPCore #1 (APB Memory access port is disabled)
4  xc7z020

有时更差的时候只看到：

DAP (Cannot open JTAG port: AP transaction error, DAP status 0x30000021)
xc7z020

我理解 Program Device 成功只能说明 PL 侧 JTAG/bitstream 下载正常，不代表 PS 侧 DAP/APU/A9 debug 一定正常。现在的经验是，只要 targets 能正常看到 APU/A9，并且 A9 #0 可以 stop，手动 ps7_init -> dow -> con 和 GUI Run As 都有机会跑通；但之前确实出现过 Run As 后 DAP/APU 变坏的现象，所以我还不能判断它到底是 launch 配置触发，还是本来 JTAG/DAP 状态偶发不稳。

我已经尝试过：

1. 关闭 Vitis/Vivado，结束 hw_server.exe、xsct.exe、xsdb.exe。
2. 板子断电重启。
3. 重新 Program Device。
4. 用 XSCT connect/targets/stop 判断 A9 是否可控。
5. 用手动 XSCT 下载 ELF，也用 GUI Run As 做对照。

想请教：

1. Vitis 2020.2 的 Run As/Launch on Hardware 是否可能因为 reset APU 或 launch 配置导致 DAP/AP transaction error？
2. 这种 “有时 targets 初始正常，但一次下载/Run As 后 DAP/APU 变坏；另一些时候手动和 Run As 又都能连续成功” 的现象，更像 JTAG 频率/USB 线/板卡供电问题，还是 Vitis launch 配置问题？
3. 是否建议在 Vivado Hardware Manager 或 XSCT 里降低 JTAG frequency？如果要降低，Vitis 侧会继承这个设置吗？
4. 有没有更标准的裸机调试流程，能避免每次靠重启和手动 XSCT 恢复？

目前手动 XSCT 和 GUI Run As 都曾经跑通，所以 application 代码、linker script、DDR、AXI-Lite 寄存器读写应该不是主问题。我主要想定位 DAP/APU debug 链路为什么会偶发进入 AP transaction error / Cannot halt processor core timeout 状态。
```

## 小结

这次真正确认了三件事：

```text
1. AP transaction timeout 不是 application 代码优先问题，而是 PS debug access 没打开。
2. Program Device 只证明 PL 可下载；A9 stop 成功才说明 PS debug 链路可用。
3. mwr/mrd 能直接验证 OCM 和 DDR；DDR 通但串口没输出，通常是串口工具没在程序运行前开始接收。
```

最有用的判断是：如果 `targets` 没有 DAP error，并且 A9 `stop` 成功，那么 XSCT 手动 `dow/con` 和 GUI `Run As` 都有希望成功。当前还没找到每次一上来都稳定进入这个状态的确定方法；它更像是 PS DAP/JTAG debug 链路偶发不稳定，失败时只能先清 hw_server、断电重启、换线/换口或降 JTAG 频率。串口没有看到字，不等于程序没跑，很多时候只是错过了输出窗口。
