# Run from any directory with:
#   vitis_hls -f path/to/run_hls_conv.tcl
#
# This project uses conv_top as the HLS top and reuses the existing GEMM core.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

file mkdir $proj_parent
cd $proj_parent

open_project -reset conv_hls
set_top conv_top

add_files -cflags "-I$src_dir" [file join $src_dir "gemm_core.cpp"]
add_files -cflags "-I$src_dir" [file join $src_dir "conv_core.cpp"]
add_files -cflags "-I$src_dir" [file join $src_dir "conv_top.cpp"]
add_files -tb -cflags "-I$src_dir" [file join $tb_dir "tb_conv.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design
csynth_design
cosim_design -rtl verilog

exit
