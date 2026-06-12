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

测试规模：

```text
Conv: CIN=3, H=W=6, COUT=4, KH=KW=3, stride=1
Attention: N=16, D=96
```

期望串口最终输出：

```text
PS-PL-DDR all-accelerator TILE8 stage5 sanity PASS
```
