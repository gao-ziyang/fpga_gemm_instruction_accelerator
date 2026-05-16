# Run from any directory with:
#   vitis_hls -f path/to/run_hls_gemm.tcl
#
# The Vitis HLS project references source files from ../hls/src and ../hls/tb.
# It does not copy those files into the generated project directory.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

cd $proj_parent
open_project mini_gemm_accel
set_top gemm_top

add_files -cflags "-I$src_dir" [file join $src_dir "gemm_core.cpp"]
add_files -cflags "-I$src_dir" [file join $src_dir "gemm_top.cpp"]
add_files -tb -cflags "-I$src_dir" [file join $tb_dir "tb_gemm.cpp"]

open_solution "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design
csynth_design

exit
