# Run from any directory with:
#   vitis_hls -f path/to/run_hls_qkv_scheduler_fixed16_csynth.tcl
#
# Fixed-shape QKV benchmark top for C-synth latency reporting.
# Shape is N=16, D=96 through qkv_scheduler_bench_top.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set proj_parent [file join $root_dir "vitis_hls_project"]
set cflags "-I$src_dir -DGZY_LAYER_GEMM_USE_SCHEDULER=1 -DGZY_ACCEL_MAX_N=16 -DGZY_ACCEL_MAX_K=96 -DGZY_ACCEL_MAX_M=96 -DGZY_ACCEL_BLOCK_N=16 -DGZY_ACCEL_BLOCK_K=32 -DGZY_ACCEL_BLOCK_M=32 -DGZY_ACCEL_BENCH_N=16 -DGZY_ACCEL_BENCH_K=96 -DGZY_ACCEL_BENCH_M=96"

file mkdir $proj_parent
cd $proj_parent

open_project -reset qkv_layer_scheduler_fixed16_csynth
set_top qkv_scheduler_bench_top

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "layer_gemm.cpp"]
add_files -cflags $cflags [file join $src_dir "qkv_projection.cpp"]
add_files -cflags $cflags [file join $src_dir "qkv_scheduler_bench_top.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csynth_design

exit
