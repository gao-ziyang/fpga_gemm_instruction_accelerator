# O7 row-unroll / BRAM banking sweep:
#   Start from the current best O2 setting:
#     TILE=14, BLOCK_N/K/M=112, LOAD_AB_PARALLEL=1
#   Keep GZY_ACCEL_FULL_BLOCK_FAST disabled so the full/boundary duplicated
#   datapath from O6 is not synthesized.  This script only measures whether
#   increasing local row banking can feed the 14x14 MAC array better.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

file mkdir $proj_parent
cd $proj_parent

proc run_case {project_name row_unroll} {
    global src_dir tb_dir

    set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=14 -DGZY_GEMM_BLOCK_M=14 -DGZY_ACCEL_BLOCK_N=112 -DGZY_ACCEL_BLOCK_K=112 -DGZY_ACCEL_BLOCK_M=112 -DGZY_ACCEL_LOAD_AB_PARALLEL=1 -DGZY_ACCEL_LOCAL_ROW_UNROLL=%d -DGZY_ACCEL_LOCAL_AB_PARALLEL=0 -DGZY_ACCEL_FULL_BLOCK_FAST=0 -DGZY_ACCEL_MAX_N=128 -DGZY_ACCEL_MAX_K=128 -DGZY_ACCEL_MAX_M=128 -DGZY_ACCEL_BENCH_N=128 -DGZY_ACCEL_BENCH_K=128 -DGZY_ACCEL_BENCH_M=128" $src_dir $tb_dir $row_unroll]

    open_project -reset $project_name
    set_top gemm_scheduler_top
    add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
    add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
    add_files -cflags $cflags [file join $src_dir "gemm_scheduler_top.cpp"]
    add_files -tb -cflags $cflags [file join $tb_dir "tb_gemm_scheduler.cpp"]

    open_solution -reset "solution1" -flow_target vivado
    set_part {xc7z020clg400-2}
    create_clock -period 10 -name default

    csim_design
    csynth_design
    cosim_design -rtl verilog

    close_project
}

run_case "accel_log14_o7a_tile14_loadab_bank4_128" 4
run_case "accel_log14_o7b_tile14_loadab_bank7_128" 7
run_case "accel_log14_o7c_tile14_loadab_bank14_128" 14

exit
