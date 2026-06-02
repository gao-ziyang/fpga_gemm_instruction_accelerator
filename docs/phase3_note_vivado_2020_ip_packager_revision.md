# Phase 3 Note：Vivado 2020.2 IP 打包时 `core_revision` 过大和 `run_ippack.tcl` 工作目录问题

## 背景

在 Phase 3 上板时，我用 Vitis HLS 2020.2 导出 `accelerator_top_axi` IP。HLS 综合完成后，理论上会在下面目录生成 Vivado 能直接识别的 IP：

```text
vitis_hls_project/accel_axi_o1_112/solution1/impl/ip
```

这个目录里最关键的文件是：

```text
component.xml
```

Vivado 的 IP Catalog 不是简单地扫 `.v` 文件，也不是看到 `hdl/verilog/accelerator_top_axi.v` 就能把它当成 IP。它需要 `component.xml` 这份描述文件。里面会写清楚：

```text
这个 IP 叫什么
有哪些接口
有哪些 RTL 文件
有哪些 driver 文件
版本号和 vendor/library/name/version 信息
```

所以第一次 Vivado 搜不到 `accelerator_top_axi` 时，问题不在 Vivado 搜索框，也不在 IP 名字拼错，而是 HLS export IP 没有完整打包成功，导致 `component.xml` 没出来。

## 当时看到的现象

HLS export 之后，`impl/ip` 目录里已经能看到一些生成物，例如：

```text
hdl/
drivers/
bd/
constraints/
run_ippack.tcl
```

但缺少：

```text
component.xml
```

这就很尴尬：RTL 好像生成了，但 Vivado 仍然不能把它当成一个完整 IP。

当时继续查 `run_ippack.tcl`，发现里面有类似：

```tcl
set Revision    "2605312313"
```

这个数字看起来不像普通版本号，更像工具按日期/时间拼出来的 revision。问题是 Vivado 2020.2 的 IP packager 对这个 revision 的数值范围比较老，遇到这么大的数字会出错。

当时的典型报错是类似：

```text
bad lexical cast
ERROR: [IMPL 213-28] Failed to generate IP.
```

结果就是：

```text
export_design 表面走了一部分
RTL 和 driver 目录生成了
但 IP packager 没收尾成功
component.xml 没生成
Vivado IP Catalog 搜不到 accelerator_top_axi
```

## `component.xml` 为什么这么重要

可以把 `component.xml` 理解成 IP 的身份证。

如果只有：

```text
accelerator_top_axi.v
```

Vivado 只知道这里有一个 Verilog 文件，但不知道它是不是标准 IP，也不知道 AXI-Lite 寄存器、AXI master 接口、driver、版本信息应该怎么整理。

有了：

```text
component.xml
```

Vivado 才能在 IP Catalog 里把它列出来，Block Design 才能通过 `Add IP` 搜到：

```text
accelerator_top_axi
```

所以判断 HLS IP 是否真的 export 成功，一个很直接的检查就是：

```text
solution1/impl/ip/component.xml 是否存在
```

如果没有，先不要急着在 Vivado 里刷新 IP Catalog，刷新也刷不出来。

## 为什么 `2605312313` 会有问题

`2605312313` 这个 revision 数字超过了旧工具内部能稳妥处理的范围。Vivado 2020.2 是 2020 年的工具，在 2026 年再用它生成 date-based revision，可能就会碰到这种老版本 IP packager 的边界。

我理解这不是我的 HLS C++ 写错，也不是 `accelerator_top_axi` 接口设计错，而是：

```text
Vivado/Vitis HLS 2020.2
  -> 自动生成过大的 core_revision / Revision
  -> 旧版 IP packager 解析失败
  -> IP 打包没完成
  -> component.xml 缺失
```

这类问题容易让人误判成“Vivado 为什么搜不到 IP”，但真正原因在更前面：IP 根本还没有被完整打包成 Vivado 认识的格式。

## 手动处理办法

当时可以手动打开：

```text
vitis_hls_project/accel_axi_o1_112/solution1/impl/ip/run_ippack.tcl
```

把：

```tcl
set Revision    "2605312313"
```

改成一个小数字，例如：

```tcl
set Revision    "1"
```

然后重新运行 IP packager：

```powershell
cd C:\Transformer\gzy_gemm_accel\vitis_hls_project\accel_axi_o1_112\solution1\impl\ip
& "C:\Xilinx\Vivado\2020.2\bin\vivado.bat" -notrace -mode batch -source ".\run_ippack.tcl"
```

成功后，目录里会出现：

```text
component.xml
xilinx_com_hls_accelerator_top_axi_1_0.zip
```

这时再回 Vivado：

```text
Tools -> Settings -> IP -> Repository
添加 solution1/impl/ip
Refresh IP Catalog
Block Design 里 Add IP 搜 accelerator_top_axi
```

就能找到 HLS IP。

## 为什么必须先 `cd` 到 `impl/ip`

这里还有第二个坑。当时我一开始直接在 PowerShell 默认目录运行：

```text
C:\WINDOWS\system32>
```

然后执行：

```powershell
C:\Xilinx\Vivado\2020.2\bin\vivado.bat -notrace -mode batch -source C:\Transformer\gzy_gemm_accel\vitis_hls_project\accel_axi_o1_112\solution1\impl\ip\run_ippack.tcl
```

结果报了类似：

```text
Failed to open handle vivado.jou
Please check access permission of directory 'C:\Windows\System32'

couldn't read directory "C:/Windows/System32/drivers/DriverData/*": permission denied
```

这个错误看起来像权限问题，但更准确地说，是工作目录错了。

`run_ippack.tcl` 里面会使用很多相对路径，例如：

```text
drivers
hdl
bd
constraints
```

脚本本来以为当前目录就是：

```text
solution1/impl/ip
```

这样 `drivers` 才会指向：

```text
solution1/impl/ip/drivers
```

但如果当前目录是：

```text
C:\Windows\System32
```

那脚本里的 `drivers` 就会被理解成：

```text
C:\Windows\System32\drivers
```

于是 Vivado 会跑去扫 Windows 系统目录里的 drivers，自然就遇到权限问题。

所以正确姿势是先进入 IP 目录：

```powershell
cd C:\Transformer\gzy_gemm_accel\vitis_hls_project\accel_axi_o1_112\solution1\impl\ip
```

再运行：

```powershell
& "C:\Xilinx\Vivado\2020.2\bin\vivado.bat" -notrace -mode batch -source ".\run_ippack.tcl"
```

这样相对路径才都落在 HLS IP 目录下。

## 这次在 HLS Tcl 里加的 workaround

为了后续不用每次手动改，我在：

```text
hls/scripts/run_hls_accel_axi_112.tcl
```

里加了一个导出后的检查。

逻辑是：

```tcl
set ip_dir [file join $proj_parent $project_name "solution1" "impl" "ip"]
set component_xml [file join $ip_dir "component.xml"]
set ip_pack_tcl [file join $ip_dir "run_ippack.tcl"]
if {![file exists $component_xml] && [file exists $ip_pack_tcl]} {
    puts "INFO: component.xml missing after export_design; applying Vivado 2020.2 core_revision workaround."

    set fp [open $ip_pack_tcl r]
    set ip_pack_data [read $fp]
    close $fp

    regsub {set Revision[ \t]+"[0-9]+"} $ip_pack_data {set Revision    "1"} ip_pack_data

    set fp [open $ip_pack_tcl w]
    puts -nonewline $fp $ip_pack_data
    close $fp

    set old_dir [pwd]
    cd $ip_dir
    ...
    exec $vivado_cmd -notrace -mode batch -source $ip_pack_tcl
    cd $old_dir
}
```

这段 workaround 做了几件事：

```text
1. export_design 后检查 component.xml 是否存在。
2. 如果 component.xml 不存在，但 run_ippack.tcl 存在，说明可能是 IP packaging 没收尾。
3. 把 run_ippack.tcl 里的 Revision 大数字替换成 1。
4. 临时 cd 到 impl/ip 目录，保证相对路径正确。
5. 调 Vivado batch mode 重新执行 run_ippack.tcl。
6. 执行完后 cd 回原目录。
```

这不是改变 HLS 算法，也不是修改 RTL。它只是帮旧版 Vivado 2020.2 把 IP 打包步骤补完整。

## 怎么判断以后又遇到了这个问题

如果以后重新导出 HLS IP 后，在 Vivado 里搜不到自己的 IP，可以按这个顺序查：

```text
1. HLS top 名字是不是正确，例如 accelerator_top_axi。
2. solution1/impl/ip 是否存在。
3. solution1/impl/ip/component.xml 是否存在。
4. 如果 component.xml 不存在，看 run_ippack.tcl 是否存在。
5. 打开 run_ippack.tcl，看 Revision 是否是很大的日期数字。
6. 检查 IP 打包日志里有没有 bad lexical cast / Failed to generate IP。
```

如果是这个问题，处理思路就是：

```text
把 Revision 改小
在 impl/ip 目录下重新运行 run_ippack.tcl
确认 component.xml 出现
回 Vivado 刷新 IP Catalog
```

## 这一点我学到什么

这次问题一开始很容易被理解成 Vivado 操作问题：为什么 Add IP 搜不到 `accelerator_top_axi`？但真正原因不是搜索框，也不是 IP repository 添加错，而是更前面的 HLS IP packaging 没完成。

我后面判断这类问题会先看：

```text
Vivado 搜不到 IP
  -> 先查 component.xml
  -> 再查 run_ippack.tcl
  -> 再查 IP packaging log
```

还有一个小经验是，运行 Xilinx 生成的 Tcl 脚本时一定注意当前工作目录。很多 generated Tcl 默认使用相对路径，双击或从 `System32` 直接调用都容易把路径带偏。

这次的结论可以简单记成：

```text
component.xml 是 Vivado 识别 HLS IP 的关键文件。
Vivado 2020.2 可能因为过大的 date-based Revision 打包失败。
run_ippack.tcl 要在 impl/ip 目录下运行，不能从 System32 直接跑。
```

