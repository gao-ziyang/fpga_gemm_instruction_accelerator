# allaccelerator_tile8_stage5_test

这个 PS 端小测试对应当前 all-accelerator baseline：

```text
hls/scripts/run_allaccelerator_baseline_tile8_stage5.tcl
vivado_board/allaccelerator_baseline_tile8_stage5/export/allaccelerator_baseline_tile8_stage5.xsa
```

它会在 DDR 中准备一组小规模数据，然后通过 AXI-Lite 启动 `accelerator_top_axi`：

```text
CONV2D -> QKV_DDR -> ATTN_SCORE_DDR -> ATTN_NORM -> ATTN_VALUE -> END
```

当前 PS app 已改成当前 bitstream 下可测的较大 sanity case：

```text
Conv: CIN=3, H=W=6, COUT=4, KH=KW=3, stride=1
Attention: N=64, D=128
```

其中 `N=64, D=128` 对应 `hls/scripts/run_allaccelerator_baseline_tile8_stage5.tcl` 里的编译上限：

```text
GZY_ACCEL_MAX_N=64
GZY_ACCEL_MAX_K=128
GZY_ACCEL_MAX_M=1024
```

总 MAC 数：

```text
Conv:      1,728
QKV:       3 * 64 * 128 * 128 = 3,145,728
Score:     64 * 64 * 128 = 524,288
Value:     64 * 64 * 128 = 524,288
Total:     4,196,032
```

期望串口最终输出：

```text
PS-PL-DDR all-accelerator TILE8 stage5 sanity PASS
```
