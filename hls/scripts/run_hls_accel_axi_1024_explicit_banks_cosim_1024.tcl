# RTL cosim for the latest explicit-banks AXI baseline at BENCH=1024.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

file mkdir $proj_parent
cd $proj_parent

set project_name "accel_axi_o1_1024_explicit_banks_cosim_1024"
set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=14 -DGZY_GEMM_BLOCK_M=14 -DGZY_ACCEL_BLOCK_N=112 -DGZY_ACCEL_BLOCK_K=112 -DGZY_ACCEL_BLOCK_M=112 -DGZY_ACCEL_LOAD_AB_PARALLEL=1 -DGZY_ACCEL_LOCAL_ROW_UNROLL=1 -DGZY_ACCEL_LOCAL_AB_PARALLEL=0 -DGZY_ACCEL_LOCAL_DOUBLE_BUFFER=0 -DGZY_ACCEL_DATAFLOW_BLOCK_OVERLAP=0 -DGZY_ACCEL_COMPUTE_PADDED_INPUTS=1 -DGZY_ACCEL_EXPLICIT_BANKS=1 -DGZY_ACCEL_FULL_BLOCK_FAST=0 -DGZY_ACCEL_FULL_ONLY=0 -DGZY_ACCEL_MAX_N=1024 -DGZY_ACCEL_MAX_K=1024 -DGZY_ACCEL_MAX_M=1024 -DGZY_ACCEL_BENCH_N=1024 -DGZY_ACCEL_BENCH_K=1024 -DGZY_ACCEL_BENCH_M=1024 -DGZY_ACCEL_MAX_INSTR=4" \
    $src_dir $tb_dir]

open_project -reset $project_name
set_top accelerator_top_axi

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_instruction.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_top_axi.cpp"]
add_files -tb -cflags $cflags [file join $tb_dir "tb_accelerator_top_axi.cpp"]

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
