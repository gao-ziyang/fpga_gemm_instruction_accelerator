# Run from any directory with:
#   vitis_hls -f path/to/run_hls_qkv.tcl
#
# This project sets qkv_top as the HLS top and routes the layer GEMM through
# the shared gemm_scheduler backend.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]
set cflags "-I$src_dir -DGZY_ACCEL_MAX_N=16 -DGZY_ACCEL_MAX_K=96 -DGZY_ACCEL_MAX_M=96 -DGZY_ACCEL_BLOCK_N=16 -DGZY_ACCEL_BLOCK_K=32 -DGZY_ACCEL_BLOCK_M=32 -DGZY_ACCEL_BENCH_N=16 -DGZY_ACCEL_BENCH_K=96 -DGZY_ACCEL_BENCH_M=96"

file mkdir $proj_parent
cd $proj_parent
open_project -reset qkv_projection_accel
set_top qkv_top

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "layer_gemm.cpp"]
add_files -cflags $cflags [file join $src_dir "qkv_projection.cpp"]
add_files -cflags $cflags [file join $src_dir "qkv_top.cpp"]
add_files -tb -cflags $cflags [file join $tb_dir "tb_qkv.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design
csynth_design
cosim_design -rtl verilog

exit
