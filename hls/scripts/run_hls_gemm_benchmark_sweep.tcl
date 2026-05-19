# Run from any directory with:
#   vitis_hls -f path/to/run_hls_gemm_benchmark_sweep.tcl
#
# This script sweeps GEMM_TILE/GEMM_BLOCK_M for a fixed benchmark:
#   C[16,96] = A[16,96] x B[96,96]

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

proc run_one_tile {proj_parent src_dir tb_dir tile block_m} {
    set project_name [format "gemm_bench_tile%d" $tile]
    set cflags [format "-I%s -DGZY_GEMM_TILE=%d -DGZY_GEMM_BLOCK_M=%d" $src_dir $tile $block_m]

    cd $proj_parent
    open_project -reset $project_name
    set_top gemm_bench_top

    add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
    add_files -cflags $cflags [file join $src_dir "gemm_bench_top.cpp"]
    add_files -tb -cflags $cflags [file join $tb_dir "tb_gemm_bench.cpp"]

    open_solution -reset "solution1" -flow_target vivado
    set_part {xc7z020clg400-2}
    create_clock -period 10 -name default

    puts [format "===== GEMM benchmark TILE=%d BLOCK_M=%d =====" $tile $block_m]
    csim_design
    csynth_design
    cosim_design -rtl verilog

    close_project
}

file mkdir $proj_parent

run_one_tile $proj_parent $src_dir $tb_dir 4 8
run_one_tile $proj_parent $src_dir $tb_dir 8 8
run_one_tile $proj_parent $src_dir $tb_dir 12 12
run_one_tile $proj_parent $src_dir $tb_dir 14 14

exit
