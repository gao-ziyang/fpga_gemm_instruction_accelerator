# XSCT script for the all-accelerator TILE8 stage5 board sanity test.
# Build allaccelerator_tile8_stage5_test before running this script.
# The script programs the PL bitstream, initializes PS/DDR, downloads the ELF,
# and starts the standalone test. Keep a UART terminal open before running it.
#
# In XSCT:
#   source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_allaccelerator_tile8_stage5_test.tcl

set psinit_file "C:/Transformer/gzy_gemm_accel/vitis_ws_allaccelerator_tile8_stage5/allaccelerator_tile8_stage5_test/_ide/psinit/ps7_init.tcl"
set elf_file    "C:/Transformer/gzy_gemm_accel/vitis_ws_allaccelerator_tile8_stage5/allaccelerator_tile8_stage5_test/Debug/allaccelerator_tile8_stage5_test.elf"
set bit_file    "C:/Transformer/gzy_gemm_accel/vitis_ws_allaccelerator_tile8_stage5/allaccelerator_tile8_stage5_test/_ide/bitstream/allaccelerator_baseline_tile8_stage5.bit"

proc stop_a9_or_continue {} {
    set rc [catch {stop} msg]
    if {$rc != 0} {
        if {[string first "Already stopped" $msg] >= 0} {
            puts "Already stopped"
        } else {
            error $msg
        }
    }
}

if {![file exists $psinit_file]} {
    error "Missing ps7_init.tcl: $psinit_file. Build the Vitis app first."
}

if {![file exists $elf_file]} {
    error "Missing ELF: $elf_file. Build the Vitis app first."
}

if {![file exists $bit_file]} {
    error "Missing bitstream: $bit_file. Run Vivado/Vitis build first."
}

puts "== connect to hw_server =="
connect

puts "== list targets =="
puts [targets]

puts "== program PL bitstream =="
targets -set -nocase -filter {name =~ "*xc7z020*"}
fpga -file $bit_file

puts "== select ARM Cortex-A9 #0 =="
targets -set -nocase -filter {name =~ "*A9*#0"}

puts "== stop A9 #0 =="
stop_a9_or_continue

puts "== initialize PS/DDR =="
configparams force-mem-access 1
source $psinit_file
ps7_init
ps7_post_config

puts "== download all-accelerator TILE8 stage5 test ELF =="
dow $elf_file

puts "== run all-accelerator TILE8 stage5 test application =="
configparams force-mem-access 0
con

puts "== done: check UART output =="
