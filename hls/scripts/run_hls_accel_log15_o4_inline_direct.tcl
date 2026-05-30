# Log15 O4 follow-up:
#   O4inline: keep the local A/B helper, but force it to inline.
#   O4_2: remove the helper call and write the combined local A/B load
#         directly inside compute_block().
#
# Both cases keep row banking disabled so this isolates the O4/O5 helper
# boundary question from the later O7 row-banking sweep.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

file mkdir $proj_parent
cd $proj_parent

proc run_case {project_name helper_inline helper_partition direct_local_ab} {
    global src_dir tb_dir

    set local_ab_parallel [expr {$direct_local_ab ? 0 : 1}]
    set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=14 -DGZY_GEMM_BLOCK_M=14 -DGZY_ACCEL_BLOCK_N=112 -DGZY_ACCEL_BLOCK_K=112 -DGZY_ACCEL_BLOCK_M=112 -DGZY_ACCEL_LOAD_AB_PARALLEL=1 -DGZY_ACCEL_LOCAL_ROW_UNROLL=1 -DGZY_ACCEL_LOCAL_AB_PARALLEL=%d -DGZY_ACCEL_LOCAL_AB_HELPER_INLINE=%d -DGZY_ACCEL_LOCAL_AB_HELPER_PARTITION=%d -DGZY_ACCEL_LOCAL_AB_DIRECT=%d -DGZY_ACCEL_FULL_BLOCK_FAST=0 -DGZY_ACCEL_MAX_N=128 -DGZY_ACCEL_MAX_K=128 -DGZY_ACCEL_MAX_M=128 -DGZY_ACCEL_BENCH_N=128 -DGZY_ACCEL_BENCH_K=128 -DGZY_ACCEL_BENCH_M=128" \
        $src_dir $tb_dir $local_ab_parallel $helper_inline $helper_partition $direct_local_ab]

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

run_case "accel_log15_o4_inline_helper_128" 1 0 0
run_case "accel_log15_o4_2_direct_localab_128" 0 0 1

exit
