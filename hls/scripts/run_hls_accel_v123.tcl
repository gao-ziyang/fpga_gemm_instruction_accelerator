# Run all non-AXI accelerator stages.

set script_dir [file normalize [file dirname [info script]]]
set ::GZY_NO_EXIT 1
source [file join $script_dir "run_hls_accel_v1_scheduler.tcl"]
source [file join $script_dir "run_hls_accel_v2_decode.tcl"]
source [file join $script_dir "run_hls_accel_v3_top.tcl"]
source [file join $script_dir "run_hls_accel_v1_scheduler_cosim_small.tcl"]
source [file join $script_dir "run_hls_accel_v2_decode_cosim_small.tcl"]
source [file join $script_dir "run_hls_accel_v3_top_cosim_small.tcl"]

unset ::GZY_NO_EXIT
exit
