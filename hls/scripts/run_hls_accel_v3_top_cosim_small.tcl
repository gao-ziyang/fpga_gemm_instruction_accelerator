# Small RTL cosim for V3 accelerator_top.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

file mkdir $proj_parent
cd $proj_parent

set project_name "accel_v3_top_tile12_128_cosim"
set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=12 -DGZY_GEMM_BLOCK_M=12 -DGZY_ACCEL_BLOCK_N=96 -DGZY_ACCEL_BLOCK_K=96 -DGZY_ACCEL_BLOCK_M=96 -DGZY_ACCEL_MAX_N=128 -DGZY_ACCEL_MAX_K=128 -DGZY_ACCEL_MAX_M=128 -DGZY_ACCEL_BENCH_N=128 -DGZY_ACCEL_BENCH_K=128 -DGZY_ACCEL_BENCH_M=128" $src_dir $tb_dir]

open_project -reset $project_name
set_top accelerator_top

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_instruction.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_top.cpp"]
add_files -tb -cflags $cflags [file join $tb_dir "tb_accelerator_top.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design
csynth_design
cosim_design -rtl verilog

close_project

if {![info exists ::GZY_NO_EXIT]} {
    exit
}
