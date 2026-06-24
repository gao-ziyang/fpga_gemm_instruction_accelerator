# Run from any directory with:
#   vitis_hls -f path/to/run_shared_gemm_tile8_split_axi_qkv_conv_fused_store_prepacked_w_pow2_norm_serial_im2col_csynth.tcl
#
# Stage-4b probe: Stage-4a plus serial Conv2D im2col. The im2col helper still
# runs on PL, but it no longer asks HLS to force II=1 on a single AXI master
# that must both read the input and write the im2col scratch.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]
set project_name "s4b_tile8_pow2_serial_im2col"
set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=8 -DGZY_GEMM_BLOCK_M=8 -DGZY_ACCEL_BLOCK_N=16 -DGZY_ACCEL_BLOCK_K=32 -DGZY_ACCEL_BLOCK_M=32 -DGZY_ACCEL_MAX_N=64 -DGZY_ACCEL_MAX_K=128 -DGZY_ACCEL_MAX_M=1024 -DGZY_ACCEL_MAX_INSTR=32 -DGZY_ACCEL_BENCH_N=16 -DGZY_ACCEL_BENCH_K=96 -DGZY_ACCEL_BENCH_M=96 -DGZY_ACCEL_SHARED_GEMM_INLINE=1 -DGZY_ACCEL_SPLIT_AXI_BUNDLES=1 -DGZY_ACCEL_QKV_FUSED_STORE=1 -DGZY_ACCEL_CONV_FUSED_STORE=1 -DGZY_ACCEL_CONV_PREPACKED_WEIGHT=1 -DGZY_ACCEL_TB_CONV_PREPACKED_WEIGHT=1 -DGZY_ACCEL_ATTN_NORM_POW2_APPROX=1 -DGZY_ACCEL_TB_ATTN_NORM_POW2_APPROX=1 -DGZY_ACCEL_CONV_IM2COL_SERIAL=1" $src_dir $tb_dir]

file mkdir $proj_parent
cd $proj_parent

open_project -reset $project_name
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
csynth_design

exit
