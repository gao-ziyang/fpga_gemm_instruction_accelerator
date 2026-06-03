# XSCT script for the 1024-capable PS-PL-DDR GEMM board test.
# Build accel_axi_1024_gemm_test before running this script.
# Use after Program Device has loaded the 1024-capable accelerator bitstream.
#
# In XSCT:
#   source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_run_gemm1024_test.tcl

set psinit_file "C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_1024_gemm_test/_ide/psinit/ps7_init.tcl"
set elf_file    "C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_1024_gemm_test/Debug/accel_axi_1024_gemm_test.elf"

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

puts "== connect to hw_server =="
connect

puts "== list targets =="
puts [targets]

puts "== select ARM Cortex-A9 #0 =="
targets -set -nocase -filter {name =~ "*A9*#0"}

puts "== stop A9 #0 =="
stop_a9_or_continue

puts "== initialize PS/DDR =="
configparams force-mem-access 1
source $psinit_file
ps7_init
ps7_post_config

puts "== download GEMM 1024-capable test ELF =="
dow $elf_file

puts "== run GEMM 1024-capable test application =="
configparams force-mem-access 0
con

puts "== done: check UART output =="
