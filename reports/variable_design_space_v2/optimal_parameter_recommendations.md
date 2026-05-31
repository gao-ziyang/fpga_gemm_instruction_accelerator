# Optimal Parameter Recommendations From V2 Model

This file is generated from the scanned design points.  It separates
model-optimal parameters into deployable/runtime-safe candidates,
compile-time full-only candidates, and Route-D-lite prototype candidates.
The DATAFLOW prototype rows are not final HLS results.

## Main Result

The strongest actionable result is that the current `N=K=M=128` test should
not keep using `BLOCK=112` as the only baseline.  In the generic HLS code,
tail blocks still execute fixed `BLOCK_N/K/M` loop bounds.  Therefore
`128` with `BLOCK=112` expands into `2 x 2 x 2` block iterations.  The model
selects `TILE=13, BLOCK=128, row_unroll=1` as the first synthesis candidate
because it removes those tail blocks while using fewer DSPs than TILE=14.

## Recommended Next HLS Cases

| Case | Purpose | Parameters | Model estimate | Why this one |
| --- | --- | --- | --- | --- |
| `O9a_square128_block128` | First real synthesis target | `T=13, B=128/128/128, R=1`; lat `84286`; LUT `46251`; DSP `169`; BRAM `65` | Generic runtime path; removes 128-shape tail-block amplification; keeps LUT margin. |
| `O9b_square224_block224_generic` | High-BRAM generic 224 check | `T=14, B=224/224/224, R=1`; lat `316918`; LUT `51288`; DSP `196`; BRAM `154` | Tests whether one-block 224 is worth the BRAM cost; still generic, so useful for DDR-style runtime path. |
| `O9c_square224_fullonly_row2` | Specialized full-only check | `T=14, B=224/224/224, R=2`; lat `253604`; LUT `38061`; DSP `196`; BRAM `168` | Compile-time fixed-shape path; verifies whether removing generic boundary/address hardware plus row_unroll=2 is actually synthesizable. |
| `D1_qkv_block_ab_pingpong_small` | Route-D-lite prototype | `T=12, B=16/96/48, R=1`; lat `12799`; LUT `44356`; DSP `144`; BRAM `60` | Small Transformer-shaped prototype; checks A/B block ping-pong overlap before touching square mainline. |

## Per-Workload Best Rows

| Workload | Generic runtime safe | Full-only static | Route-D-lite prototype |
| --- | --- | --- | --- |
| `square_128` | `T=13, B=128/128/128, R=1`; lat `84286`; LUT `46251`; DSP `169`; BRAM `65` | `T=8, B=128/128/128, R=4`; lat `111974`; LUT `26847`; DSP `64`; BRAM `128` | `T=13, B=128/128/128, R=1`; lat `67652`; LUT `49219`; DSP `169`; BRAM `91` |
| `square_224` | `T=13, B=224/224/224, R=1`; lat `381391`; LUT `46251`; DSP `169`; BRAM `143` | `T=14, B=224/224/224, R=2`; lat `253604`; LUT `38061`; DSP `196`; BRAM `168` | `T=13, B=224/224/224, R=1`; lat `329757`; LUT `49219`; DSP `169`; BRAM `195` |
| `square_256` | `T=13, B=256/256/256, R=1`; lat `514423`; LUT `46251`; DSP `169`; BRAM `195` | `T=8, B=256/256/256, R=4`; lat `713318`; LUT `26847`; DSP `64`; BRAM `192` | `T=13, B=256/256/256, R=1`; lat `446887`; LUT `49219`; DSP `169`; BRAM `273` |
| `qkv_16x96x96` | `T=10, B=16/96/48, R=2`; lat `17394`; LUT `45643`; DSP `100`; BRAM `60` | `T=8, B=16/96/32, R=2`; lat `18040`; LUT `18177`; DSP `64`; BRAM `48` | `T=12, B=16/96/48, R=1`; lat `12799`; LUT `44356`; DSP `144`; BRAM `60` |
| `qkt_16x96x16` | `T=8, B=16/96/16, R=2`; lat `3019`; LUT `34859`; DSP `64`; BRAM `48` | `T=8, B=16/96/16, R=2`; lat `3007`; LUT `18177`; DSP `64`; BRAM `48` | `T=8, B=16/96/16, R=1`; lat `2183`; LUT `27062`; DSP `64`; BRAM `40` |
| `sv_16x16x96` | `T=8, B=16/16/32, R=2`; lat `4689`; LUT `34859`; DSP `64`; BRAM `48` | `T=8, B=16/16/32, R=2`; lat `4677`; LUT `18177`; DSP `64`; BRAM `48` | `T=8, B=16/16/32, R=1`; lat `3671`; LUT `27062`; DSP `64`; BRAM `40` |
| `ffn_up_16x96x384` | `T=10, B=16/96/128, R=2`; lat `69085`; LUT `45643`; DSP `100`; BRAM `60` | `T=8, B=16/96/64, R=2`; lat `72161`; LUT `18177`; DSP `64`; BRAM `48` | `T=12, B=16/96/48, R=1`; lat `51195`; LUT `44356`; DSP `144`; BRAM `60` |
| `ffn_down_16x384x96` | `T=10, B=16/384/48, R=2`; lat `63067`; LUT `45643`; DSP `100`; BRAM `60` | `T=8, B=16/384/32, R=2`; lat `66148`; LUT `18177`; DSP `64`; BRAM `48` | `T=12, B=16/384/48, R=1`; lat `45838`; LUT `44356`; DSP `144`; BRAM `60` |

## Interpretation

- `O9a_square128_block128` is the most important next case.  It changes only
  the compile-time tile/block shape and does not depend on DATAFLOW.
- `O9b_square224_block224_generic` may be BRAM-heavy, but it tests the same
  idea on 224 without relying on full-only specialization.
- `O9c_square224_fullonly_row2` is a specialized fixed-shape path, not a
  replacement for the generic DDR-facing baseline.
- `D1_qkv_block_ab_pingpong_small` should only be used to read the HLS
  DATAFLOW report and resource growth.  It should not be promoted to
  TILE=14/BLOCK=112 or full square GEMM until overlap is proven.

## Stop Criteria

- If `O9a` exceeds `53,200` LUT or fails timing badly, do not expand the
  block-size route without a new model correction.
- If `D1` does not show real load/compute overlap in the HLS report, stop
  Route D implementation work and keep it as analysis only.
- If any candidate duplicates DSP from about 196 to about 392, mark it
  exploration-only immediately.
