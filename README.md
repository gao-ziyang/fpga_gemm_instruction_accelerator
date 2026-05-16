# gzy_gemm_accel

This is Gao Ziyang's clean HLS workspace for the adviser GEMM task.

The first milestone is intentionally small:

```text
C[4][4] = A[4][4] x B[4][4]
```

The goal is to understand and verify:

- `ap_int`
- `ARRAY_PARTITION`
- `UNROLL`
- `PIPELINE`
- Vitis HLS C simulation
- Python golden baseline

## Layout

```text
gzy_gemm_accel/
  hls/
    src/          # HLS C++ design source
    tb/           # HLS C simulation testbench
    scripts/      # Tcl scripts for Vitis HLS
  python/
    golden/       # Python reference baselines
  tests/
    data/         # Optional generated test vectors
  docs/           # Notes and reports
  reports/        # Copied summaries, not raw tool output
  vitis_hls_project/
    mini_gemm_accel/  # Vitis HLS generated project
```

Keep source files in `hls/src` and `hls/tb`. The Vitis HLS project should reference these external files instead of copying them into the generated project directory.

## First Run

Run the Python baseline:

```bash
python3 gzy_gemm_accel/python/golden/gemm_4x4_baseline.py
```

Run Vitis HLS with the Tcl script from Windows or a shell that can access Vitis HLS:

```bat
C:\xilinx\Vitis_HLS\2020.2\bin\vitis_hls.bat -f C:\Transformer\gzy_gemm_accel\hls\scripts\run_hls_gemm.tcl
```

Target settings:

```text
Top function : gemm_top
Part         : xc7z020clg400-2
Clock        : 10 ns
Flow target  : Vivado IP
```
