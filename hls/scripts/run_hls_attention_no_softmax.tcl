# Run from any directory with:
#   vitis_hls -f path/to/run_hls_attention_no_softmax.tcl
#
# Stage 3: synthesize attention_no_softmax_top, which validates
# QKV -> QK^T -> quantized Score * V.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

file mkdir $proj_parent
cd $proj_parent

open_project -reset attention_no_softmax_hls
set_top attention_no_softmax_top

add_files -cflags "-I$src_dir" [file join $src_dir "gemm_core.cpp"]
add_files -cflags "-I$src_dir" [file join $src_dir "qkv_projection.cpp"]
add_files -cflags "-I$src_dir" [file join $src_dir "attention_core.cpp"]
add_files -cflags "-I$src_dir" [file join $src_dir "attention_top.cpp"]
add_files -tb -cflags "-I$src_dir" [file join $tb_dir "tb_attention.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design
csynth_design
cosim_design -rtl verilog

exit
