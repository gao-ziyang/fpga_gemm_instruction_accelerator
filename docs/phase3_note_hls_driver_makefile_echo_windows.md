# Phase 3 Note：Vitis HLS 2020.2 生成的 Makefile 中 `#echo` 在 Windows 下为什么会失败

## 背景

在 Phase 3 上板流程中，我用 Vitis HLS 2020.2 导出了 `accelerator_top_axi` IP。这个 IP 除了 RTL 以外，还会自动生成一份 PS 端可用的 HLS driver，例如：

```text
vitis_hls_project/accel_axi_o1_112/solution1/impl/ip/drivers/accelerator_top_axi_v1_0/src/
```

里面会有：

```text
xaccelerator_top_axi.h
xaccelerator_top_axi.c
Makefile
```

后续在 Xilinx Vitis 里 build platform 时，Vitis 会编译 BSP 和这些 IP driver。问题就出现在 HLS 自动生成的 driver `Makefile` 里。

当时 Vitis platform build 报错类似：

```text
process_begin: CreateProcess(NULL, #echo "Copying 'xaccelerator_top_axi.h' to '../../../include/xaccelerator_top_axi.h'", ...) failed.
make (e=2): 系统找不到指定的文件。
make[2]: *** [Makefile:40: ../../../include/xaccelerator_top_axi.h] Error 2
```

这个错误不是 `accelerator_top_axi` 的算法错误，也不是 Vivado Block Design 接线错误，而是 Makefile recipe 在 Windows CMD 下的语法兼容问题。

## Makefile 里的 recipe 是什么

Makefile 里一条规则通常长这样：

```makefile
target: dependency
	command
```

注意 `command` 前面通常是一个 Tab。只要一行在规则下面并且以 Tab 开头，`make` 就会把它当作 recipe，也就是要执行的命令。

例如：

```makefile
../../../include/xaccelerator_top_axi.h: xaccelerator_top_axi.h
	cp $< $@
```

这里 `cp $< $@` 是 recipe。`make` 自己不会执行 `cp`，它会把这行命令交给 shell 去执行。

关键点是：

```text
Makefile 顶层注释:
  行首 # 是 make 的注释。

Recipe 里的 #:
  如果这一行已经以 Tab 开头，它首先是 recipe。
  后续 # 怎么解释，要看执行它的 shell。
```

也就是说：

```makefile
# 这是 Makefile 注释

target:
	#echo "copying"
```

上面两处 `#` 不在同一层。第一处是 Makefile 注释；第二处是 recipe 内容，会被交给 shell。

## Linux shell 怎么理解 `#echo`

在 Linux / POSIX shell 中，`#` 通常表示注释开始。比如：

```sh
#echo "Copying file"
```

这行会被 shell 当作注释，不执行任何命令。

所以如果 Makefile recipe 是：

```makefile
target:
	#echo "Copying file"
	cp src dst
```

在 Linux 下，`make` 会把 `#echo "Copying file"` 交给 `/bin/sh`，而 `/bin/sh` 会把它当注释忽略。于是不会报错。

这也是为什么同样的 generated Makefile 在 Linux 环境里可能看起来没有问题。

## Windows CMD 怎么理解 `#echo`

Windows `cmd.exe` 里，`#` 不是注释符。

CMD 里的注释常见写法是：

```bat
REM this is a comment
:: this is often used like a comment in batch files
```

所以对 Windows CMD 来说：

```bat
#echo "Copying file"
```

不是注释，而是要执行一个叫：

```text
#echo
```

的命令。

系统里当然没有 `#echo.exe`，所以就会报：

```text
CreateProcess(NULL, #echo ...)
make (e=2): 系统找不到指定的文件。
```

这里的 `系统找不到指定的文件` 指的不是找不到 `xaccelerator_top_axi.h`，而是找不到 `#echo` 这个命令。

## `@echo` 又是什么意思

在 Makefile 的 recipe 中，命令前面的 `@` 是 `make` 的特殊前缀，作用是：

```text
执行这条命令，但不要把命令本身打印出来。
```

例如：

```makefile
target:
	@echo "Copying file"
```

这里 `make` 会先处理 `@`，然后把真正的命令交给 shell：

```text
echo "Copying file"
```

所以：

```text
Linux /bin/sh:
  收到 echo "Copying file"，正常打印。

Windows CMD:
  收到 echo "Copying file"，也正常打印。
```

因此 `@echo` 是跨 Linux shell 和 Windows CMD 都比较稳的写法。

## `#echo` 和 `@echo` 对比

| Makefile recipe 写法 | Make 先怎么处理 | Linux / POSIX shell 看到什么 | Windows CMD 看到什么 | 结果 |
| --- | --- | --- | --- | --- |
| `#echo "Copying"` | recipe，交给 shell | 注释，忽略 | 命令名是 `#echo` | Linux 通常没事，Windows 失败 |
| `@echo "Copying"` | `@` 被 make 去掉，并禁止打印命令本身 | `echo "Copying"` | `echo "Copying"` | Linux/Windows 都能执行 |

这个表是这次问题的核心。

## 为什么 Vitis HLS 2020.2 会生成这种东西

这是旧版 Vitis HLS 自动生成 driver Makefile 时留下的兼容性问题。它生成的 Makefile 大致希望表达：

```text
这里本来只想留一条说明性的 echo，或者在 Linux shell 下让它作为注释不执行。
```

但在 Windows Vitis 2020.2 的 BSP build 环境中，`make` 使用的 shell/命令执行方式和 Linux 不一样，导致 `#echo` 没有被当作注释，而是被当作命令执行。

所以这个问题不是我自己写 Tcl 写错，也不是 HLS C++ top 写错，而是：

```text
Vitis HLS 2020.2 generated driver Makefile
  + Windows CMD / Windows make execution behavior
  -> #echo 被当成命令
  -> platform build 失败
```

## 这次怎么解决

我没有手动修改生成目录作为长期方案，而是在 HLS Tcl 导出 IP 后加了一个 post-export workaround。

当前脚本位置：

```text
hls/scripts/run_hls_accel_axi_112.tcl
```

相关逻辑是：

```tcl
set driver_makefile [file join $ip_dir "drivers" "accelerator_top_axi_v1_0" "src" "Makefile"]
if {[file exists $driver_makefile]} {
    puts "INFO: applying Windows CMD workaround to generated HLS driver Makefile."

    set fp [open $driver_makefile r]
    set makefile_data [read $fp]
    close $fp

    set makefile_data [string map [list "\t#echo" "\t@echo"] $makefile_data]

    set fp [open $driver_makefile w]
    puts -nonewline $fp $makefile_data
    close $fp
}
```

这段 Tcl 的含义是：

```text
1. 找到 HLS export_design 自动生成的 driver Makefile。
2. 读取 Makefile 内容。
3. 只把 recipe 开头的 Tab + #echo 替换为 Tab + @echo。
4. 写回 Makefile。
```

替换前：

```makefile
	#echo "Copying 'xaccelerator_top_axi.h' to '../../../include/xaccelerator_top_axi.h'"
```

替换后：

```makefile
	@echo "Copying 'xaccelerator_top_axi.h' to '../../../include/xaccelerator_top_axi.h'"
```

这样 Vitis platform build 时，Windows CMD 最终执行的是正常的 `echo` 命令，不会再尝试执行 `#echo`。

## 为什么不是直接删掉这一行

理论上也可以直接删掉 `#echo` 这一行，因为它只是打印提示，不是复制文件的核心命令。

但我选择改成 `@echo`，原因是：

```text
1. 改动更小，只修正语法兼容性。
2. 保留原本想表达的 Copying 提示。
3. 不影响后面的 cp/copy/install 逻辑。
4. Linux 和 Windows 都能接受。
```

也就是说，这个 workaround 不是改 driver 功能，而是把生成 Makefile 里一个不跨平台的提示命令改成跨平台可执行的提示命令。

## 如何判断以后又遇到了同类问题

如果后续重新导出 HLS IP、重新建 Vitis platform 时又看到类似：

```text
CreateProcess(NULL, #echo ...)
make (e=2): 系统找不到指定的文件。
```

可以优先判断：

```text
不是头文件真的丢了
不是 Vitis 找不到 IP driver
而是 Makefile 里有 Windows 不能识别的 #echo 命令
```

检查路径一般是：

```text
vitis_hls_project/<hls_project>/solution1/impl/ip/drivers/<ip_name>_v1_0/src/Makefile
```

搜索：

```text
#echo
```

如果存在，把 recipe 行里的：

```makefile
	#echo ...
```

改成：

```makefile
	@echo ...
```

再重新 build platform。

## 这一点我学到什么

这次问题让我意识到，HLS 上板不只是 C++ 综合和 Vivado 接线，还会遇到 generated software driver、BSP、Makefile、Windows/Linux shell 差异这些工程细节。

这类问题的判断方法是：

```text
先看报错发生在哪一层：
  HLS C-sim / C-synth?
  Vivado IP packaging?
  Vivado Block Design?
  Vitis platform BSP build?
  Application ELF download?

再看报错对象是什么：
  算法数值?
  RTL/IP?
  AXI 接口?
  driver Makefile?
  JTAG/PS debug?
```

这次 `#echo` 属于：

```text
Vitis platform BSP build
  -> HLS generated driver Makefile
  -> Windows CMD 兼容性
```

因此它不说明 `accelerator_top_axi` 设计错了，只说明旧版工具生成的 driver Makefile 需要一个小补丁才能在当前 Windows Vitis 2020.2 环境稳定构建。

