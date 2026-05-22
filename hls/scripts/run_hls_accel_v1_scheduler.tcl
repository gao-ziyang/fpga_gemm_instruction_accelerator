# Run from any directory with:
#   vitis_hls -f path/to/run_hls_accel_v1_scheduler.tcl

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

file mkdir $proj_parent
cd $proj_parent

set project_name "accel_v1_scheduler_tile14_1024"
set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=14 -DGZY_GEMM_BLOCK_M=14 -DGZY_ACCEL_BLOCK_N=112 -DGZY_ACCEL_BLOCK_K=112 -DGZY_ACCEL_BLOCK_M=112 -DGZY_ACCEL_LOAD_AB_PARALLEL=1 -DGZY_ACCEL_LOCAL_ROW_UNROLL=2" $src_dir $tb_dir]

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

# 1024^3 的 RTL cosim 会仿真数千万周期，适合作为最终长跑，
# 日常迭代用 run_hls_accel_v1_scheduler_cosim_small.tcl 做 RTL 等价验证。

close_project

if {![info exists ::GZY_NO_EXIT]} {
    exit
}
