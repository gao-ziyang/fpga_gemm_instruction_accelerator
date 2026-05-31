# Phase 1 / 迭代日志 001：先把最小 4x4 GEMM 跑通

## 我这一版想解决什么

刚开始我对 HLS 的很多东西还不熟，所以第一步没有直接写很大的 GEMM，而是先写一个最小的：

```text
C[4,4] = A[4,4] x B[4,4]
```

这一版我主要想把几件事跑通：

1. HLS 里怎么写 `ap_int<8>` 和 `ap_int<32>`。
2. 怎么写一个可以综合的 C++ GEMM 函数。
3. 怎么写 `gemm_top()` 作为 HLS 顶层。
4. 怎么写 `tb_gemm.cpp` 做 C simulation。
5. 写 Python baseline 对照结果。
6. 怎么用 Tcl 自动跑 C-sim 和 C-synth。

## 我改了哪些文件

| 文件 | 作用 |
| --- | --- |
| `hls/src/gemm_types.h` | 放 GEMM 的尺寸和数据类型。 |
| `hls/src/gemm_core.h` | 声明 GEMM 相关函数。 |
| `hls/src/gemm_core.cpp` | 写最小 `4x4` GEMM 核心。 |
| `hls/src/gemm_top.cpp` | 写 HLS 顶层函数和接口 pragma。 |
| `hls/tb/tb_gemm.cpp` | 写 C++ testbench。 |
| `python/golden/gemm_4x4_baseline.py` | 写 Python baseline。 |
| `hls/scripts/run_hls_gemm.tcl` | 用 Tcl 自动跑 HLS。 |

## 我学到的东西

### ap_int

这一版我用了：

```cpp
typedef ap_int<8>  gemm_data_t;
typedef ap_int<32> gemm_acc_t;
```

我理解这里的 `ap_int<8>` 就是 HLS 里的任意位宽有符号整数，适合表达硬件里的精确 bit 宽。`INT8 x INT8` 后面会累加，所以输出先用 `INT32`，这样比较接近常见量化推理里的 `int8` 输入、`int32` accumulate。

现在我也意识到，`INT32` 不是最省资源的选择。后续如果做资源优化，可以再讨论 accumulator 到底要多少位，比如 `ap_int<24>` 之类。

### ARRAY_PARTITION / UNROLL / PIPELINE

这一版开始接触这几个 HLS 指令：

```cpp
#pragma HLS ARRAY_PARTITION
#pragma HLS UNROLL
#pragma HLS PIPELINE II=1
```

我现在的理解是：

```text
ARRAY_PARTITION：让小数组更像寄存器阵列，方便并行访问。
UNROLL：把小循环在空间上展开，生成更多并行硬件。
PIPELINE：让循环在时间上流水，每隔 II 个周期启动新一轮迭代。
```

这个阶段我还只是先用起来，后面通过综合报告再慢慢看它们对资源和 II 的影响。

### 为什么要写 gemm_top

`gemm_top.cpp` 里面的顶层接口写法是：

```cpp
#pragma HLS INTERFACE ap_memory port=A
#pragma HLS INTERFACE ap_memory port=B
#pragma HLS INTERFACE ap_memory port=C
#pragma HLS INTERFACE ap_ctrl_hs port=return
```

我的理解是：`A/B/C` 是数组，HLS 需要知道这些数组端口按 memory-like 接口生成；`ap_ctrl_hs` 会给这个函数生成类似 `start/done/idle/ready` 的握手控制信号。

`N/K/M` 这类标量后面如果加进去，一般可以先让 HLS 自动推断成普通输入端口。真正要上板的时候，最终系统顶层再认真规划 AXI-Lite、AXI Master 或 DMA。

### extern "C"

顶层函数外面加：

```cpp
extern "C" {
    void gemm_top(...)
}
```

我的理解是：这是 C++ 语法，作用是让函数名不要被 C++ 编译器改名，导出的顶层名字更稳定，也方便 HLS 工具识别和生成 IP。

## 验证过程

### Python baseline

命令：

```bash
cd /mnt/c/Transformer/gzy_gemm_accel
python3 python/golden/gemm_4x4_baseline.py
```

关键输出：

```text
[PY] C = A x B:
     -14       16      -44      120
    -192      228     -170       16
     -90       96      -76        8
     190     -184       84       88
```

### HLS C simulation

命令：

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
```

关键输出：

```text
[TB] C from HLS:
     -14      16     -44     120
    -192     228    -170      16
     -90      96     -76       8
     190    -184      84      88
[TB] Golden:
     -14      16     -44     120
    -192     228    -170      16
     -90      96     -76       8
     190    -184      84      88
[TB] PASS
INFO: [SIM 1] CSim done with 0 errors.
```

### HLS C synthesis

综合摘要：

```text
Target clock     : 10.00 ns
Estimated clock  : 6.722 ns
Latency          : 56 cycles
BRAM_18K         : 0
DSP              : 2
FF               : 502
LUT              : 485
Estimated Fmax   : about 148.76 MHz
```

## 这一版的问题和后续想法

这一版虽然能跑通，但它只支持固定 `4x4`，离真正的 GEMM 还很远。后续至少要支持运行时传入 `N/K/M`，也要支持大矩阵拆成多个 tile。

另外，这一版 BRAM 是 0，说明小数组基本被 HLS 放在寄存器或 LUT 逻辑里了。后面如果矩阵变大，就必须开始考虑片上 buffer、BRAM 端口和数据复用问题。
