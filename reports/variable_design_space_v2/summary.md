# Variable GEMM Design Space V2 Summary

Total scanned points: `303120`
Resource-feasible points: `146700`
Pareto points: `126`
Figure generation: `skipped by --no-figures`

## Best Feasible Candidates

| Workload | Arch | tile | block N/K/M | row | latency | LUT | DSP | BRAM | reason |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| conv_16x27x4 | block_ab_pingpong_analysis | 10 | 16/27/4 | 1 | 589 | 35164 | 100 | 32 | fits resource limits; local feeding dominated |
| conv_196x27x32 | block_abc_dataflow_analysis | 11 | 44/27/32 | 1 | 11475 | 51971 | 121 | 66 | fits resource limits; has tail blocks; local feeding dominated; exploration only |
| conv_64x27x16 | block_abc_dataflow_analysis | 10 | 16/27/16 | 1 | 2809 | 47512 | 100 | 60 | fits resource limits; local feeding dominated; exploration only |
| ffn_down_16x384x96 | block_ab_pingpong_analysis | 12 | 16/384/48 | 1 | 45838 | 44356 | 144 | 60 | fits resource limits; local feeding dominated |
| ffn_up_16x96x384 | block_abc_dataflow_analysis | 11 | 16/96/64 | 1 | 49310 | 51971 | 121 | 66 | fits resource limits; local feeding dominated; exploration only |
| qkt_16x96x16 | block_ab_pingpong_analysis | 8 | 16/96/16 | 1 | 2183 | 27062 | 64 | 40 | fits resource limits; local feeding dominated |
| qkv_16x96x96 | block_ab_pingpong_analysis | 12 | 16/96/48 | 1 | 12799 | 44356 | 144 | 60 | fits resource limits; local feeding dominated |
| square_128 | block_ab_pingpong_analysis | 13 | 128/128/128 | 1 | 67652 | 49219 | 169 | 91 | fits resource limits; local feeding dominated |
| square_224 | full_only_static | 14 | 224/224/224 | 2 | 253604 | 38061 | 196 | 168 | fits resource limits; removes generic tail/boundary/address mux penalties |
| square_256 | block_ab_pingpong_analysis | 13 | 256/256/256 | 1 | 446887 | 49219 | 169 | 273 | fits resource limits; local feeding dominated |
| sv_16x16x96 | block_abc_dataflow_analysis | 8 | 16/16/32 | 1 | 2929 | 39254 | 64 | 48 | fits resource limits; local feeding dominated; exploration only |

## Important Interpretation

- O1 remains the deployable baseline when the model picks O1-like points near the current constraints.
- O2-like row banking can improve latency, but the calibrated LUT penalty often pushes it beyond 53,200 LUT.
- The 224 full-only resource drop is modeled explicitly as removed generic tail, boundary check, address mux, and fallback hardware.
- Route-D DATAFLOW candidates are analysis/prototype candidates unless they fit LUT/DSP/BRAM and are later confirmed by HLS reports.
