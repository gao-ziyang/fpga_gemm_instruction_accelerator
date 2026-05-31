# Variable Design Space V2 Model Assumptions

## Scope

This model is a design-space estimator, not an HLS replacement.  It separates
ideal lower-bound cycles, current HLS loop-schedule cycles, and calibrated
resource/latency estimates.

## Workloads

The built-in workload list includes square GEMM, Transformer-like QKV/QK^T/SV/FFN
shapes, and CNN im2col-like shapes.  Workloads are represented by a `Workload`
dataclass so new shapes can be added in one list.

## Latency models

- Ideal roofline: `max(N*K*M/(tile*tile), external_bytes/ddr_bytes_per_cycle)`.
- Ideal no-overlap: external load + idealized local/internal + external store.
- HLS loop schedule: current scheduler-style `tripcount x II`, using fixed
  `block_n/block_k/block_m` loop bounds instead of shortening tail loops.
- Calibrated latency: HLS loop total plus calibrated control/gap terms.  For
  DATAFLOW analysis variants, the model also reports an optimistic overlap
  estimate, but these variants are marked exploration unless resources fit.

## Resource models

BRAM is modeled with both raw bits and banked BRAM18 estimates.  The banked
estimate accounts for tile column banking and row_unroll banking.  For O1-like
`tile=14, block=112, row_unroll=1`, the model reproduces:

```text
A_buf = 14 BRAM18K
B_buf = 14 BRAM18K
C_buf = 28 BRAM18K
total = 56 BRAM18K
```

BRAM limit used for `resource_feasible`: `280`.
Compact block scan: `True`.
Compact block candidates per dimension: `3` when
compact scan is enabled.  Use `--full-block-scan` for the full fixed block list.

## 224 full-block resource calibration

The model explicitly separates the resource penalty of generic runtime paths:

- `boundary_check_lut_penalty`
- `address_mux_lut_penalty`
- `generic_tail_lut_penalty`
- `fallback_path_lut_penalty`
- corresponding FF terms

This is calibrated from `O1_224_generic` vs `O6c_fullonly_224`.  The observed
drop is not treated as a plotting artifact; it is modeled as a real generic
tail/boundary/address/fallback hardware cost.  Calibration notes:

- O1 is used as the deployable baseline calibration point.
- row_unroll=2 LUT penalty calibrated from O2-O1: 18523.
- row_unroll=4 LUT penalty calibrated from O7a-O1: 33696.
- local A/B helper penalty calibrated from O4-O1.
- O8 local double-buffer penalty calibrated from O8a-O1.
- runtime full/generic fallback penalty calibrated from O6a-O1.
- 224 full-only calibration: generic tail/boundary/address path accounts for about 61.9% LUT and 15.4% FF of O1_224_generic.

## Known limitations

- LUT/FF are approximate and intended for filtering and ranking, not signoff.
- DATAFLOW overlap points are analysis points.  The main TILE=14/BLOCK=112 line
  should not be changed directly without a small prototype report.
- The model is conservative about O8-style helper/DATAFLOW over local arrays,
  because experiments showed heavy FIFO/control costs.
- INT16 support is structural; it uses conservative scaling rather than direct
  HLS calibration.
