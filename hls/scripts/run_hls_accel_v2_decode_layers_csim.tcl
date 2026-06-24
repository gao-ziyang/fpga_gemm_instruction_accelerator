# Run from any directory with:
#   vitis_hls -f path/to/run_hls_accel_v2_decode_layers_csim.tcl
#
# Functional C simulation for the extended 64-bit instruction decode path.
# The stream covers QKV, attention-score, and conv-as-GEMM opcodes.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]
set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=4 -DGZY_GEMM_BLOCK_M=8 -DGZY_ACCEL_BLOCK_N=16 -DGZY_ACCEL_BLOCK_K=32 -DGZY_ACCEL_BLOCK_M=32 -DGZY_ACCEL_MAX_N=64 -DGZY_ACCEL_MAX_K=128 -DGZY_ACCEL_MAX_M=1024 -DGZY_ACCEL_MAX_INSTR=8 -DGZY_ACCEL_BENCH_N=16 -DGZY_ACCEL_BENCH_K=96 -DGZY_ACCEL_BENCH_M=96" $src_dir $tb_dir]

file mkdir $proj_parent
cd $proj_parent

open_project -reset accel_v2_decode_layers_csim
set_top instruction_decode_top

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_instruction.cpp"]
add_files -cflags $cflags [file join $src_dir "instruction_decode_top.cpp"]
add_files -tb -cflags $cflags [file join $tb_dir "tb_instruction_decode_layers.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design

exit
