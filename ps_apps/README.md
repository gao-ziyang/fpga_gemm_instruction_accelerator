# PS application source snapshots

`vitis_ws/` is intentionally ignored because it contains local Vitis GUI projects and generated files.

This directory keeps small board-facing PS application source snapshots that were copied into Vitis `src/helloworld.c` during bring-up. They are tracked so the validated board test logic is preserved without committing the whole Vitis workspace.

Current snapshots:

```text
accel_axi_112_gemm_test/helloworld.c
  PS-PL-DDR GEMM sanity test for accelerator_top_axi.
  The current checked-in version uses N=112, K=112, M=112.
```
