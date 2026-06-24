# Run from any directory with:
#   vitis_hls -f path/to/run_hls_conv_scheduler_csim.tcl
#
# This C simulation uses conv_scheduler_top and routes layer_gemm through
# gemm_scheduler with small layer-level capacities.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]
set cflags "-I$src_dir -DGZY_CONV_TOP_FN=conv_scheduler_top -DGZY_ACCEL_MAX_N=16 -DGZY_ACCEL_MAX_K=96 -DGZY_ACCEL_MAX_M=96 -DGZY_ACCEL_BLOCK_N=16 -DGZY_ACCEL_BLOCK_K=32 -DGZY_ACCEL_BLOCK_M=32 -DGZY_ACCEL_BENCH_N=16 -DGZY_ACCEL_BENCH_K=96 -DGZY_ACCEL_BENCH_M=96"

file mkdir $proj_parent
cd $proj_parent

open_project -reset conv_layer_scheduler_csim
set_top conv_scheduler_top

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "layer_gemm.cpp"]
add_files -cflags $cflags [file join $src_dir "conv_core.cpp"]
add_files -cflags $cflags [file join $src_dir "conv_scheduler_top.cpp"]
add_files -tb -cflags $cflags [file join $tb_dir "tb_conv.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design

exit
