# Run from any directory with:
#   vitis_hls -f path/to/run_hls_attention_scheduler_csim.tcl
#
# This C simulation uses explicit scheduler top wrappers for attention-score,
# no-softmax attention, and full row-normalized attention.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]
set cflags "-I$src_dir -DGZY_ATTENTION_SCORE_TOP_FN=attention_score_scheduler_top -DGZY_ATTENTION_NO_SOFTMAX_TOP_FN=attention_no_softmax_scheduler_top -DGZY_ATTENTION_TOP_FN=attention_scheduler_top -DGZY_ACCEL_MAX_N=16 -DGZY_ACCEL_MAX_K=96 -DGZY_ACCEL_MAX_M=96 -DGZY_ACCEL_BLOCK_N=16 -DGZY_ACCEL_BLOCK_K=32 -DGZY_ACCEL_BLOCK_M=32 -DGZY_ACCEL_BENCH_N=16 -DGZY_ACCEL_BENCH_K=96 -DGZY_ACCEL_BENCH_M=96"

file mkdir $proj_parent
cd $proj_parent

open_project -reset attention_layer_scheduler_csim
set_top attention_scheduler_top

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "layer_gemm.cpp"]
add_files -cflags $cflags [file join $src_dir "qkv_projection.cpp"]
add_files -cflags $cflags [file join $src_dir "attention_core.cpp"]
add_files -cflags $cflags [file join $src_dir "attention_scheduler_top.cpp"]
add_files -tb -cflags $cflags [file join $tb_dir "tb_attention.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design

exit
