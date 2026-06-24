# Run from any directory with:
#   vitis_hls -f path/to/run_hls_accel_axi_operator_descriptor_csim.tcl
#
# Functional C simulation for the AXI top using multi-word operator
# descriptors: full Conv2D, QKV with K_T writeback, attention score, norm,
# and attention value.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]
set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=4 -DGZY_GEMM_BLOCK_M=8 -DGZY_ACCEL_BLOCK_N=16 -DGZY_ACCEL_BLOCK_K=32 -DGZY_ACCEL_BLOCK_M=32 -DGZY_ACCEL_MAX_N=64 -DGZY_ACCEL_MAX_K=128 -DGZY_ACCEL_MAX_M=1024 -DGZY_ACCEL_MAX_INSTR=32 -DGZY_ACCEL_BENCH_N=16 -DGZY_ACCEL_BENCH_K=96 -DGZY_ACCEL_BENCH_M=96" $src_dir $tb_dir]

file mkdir $proj_parent
cd $proj_parent

open_project -reset accel_axi_operator_descriptor_csim
set_top accelerator_top_axi

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_instruction.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_top_axi.cpp"]
add_files -tb -cflags $cflags [file join $tb_dir "tb_accelerator_top_axi_operator_descriptor.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design

exit
