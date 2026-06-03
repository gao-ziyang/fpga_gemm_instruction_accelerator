# XSCT repeat script for the enhanced 112 GEMM board test.
# Build accel_axi_112_gemm_test before running this script.
# Use after Program Device has loaded accel_axi_112.bit.
#
# In XSCT:
#   source C:/Transformer/gzy_gemm_accel/scripts/xsct/xsct_repeat_gemm112_test.tcl

set repeat_count 3
set run_wait_ms 12000

set psinit_file "C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_gemm_test/_ide/psinit/ps7_init.tcl"
set elf_file    "C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_gemm_test/Debug/accel_axi_112_gemm_test.elf"

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

puts "== initial targets =="
puts [targets]

puts "== load ps7 init script once =="
source $psinit_file

for {set i 1} {$i <= $repeat_count} {incr i} {
    puts ""
    puts "========== GEMM 112 ELF run $i / $repeat_count =========="

    puts "== select ARM Cortex-A9 #0 =="
    targets -set -nocase -filter {name =~ "*A9*#0"}

    puts "== stop A9 #0 =="
    stop_a9_or_continue

    puts "== initialize PS/DDR =="
    configparams force-mem-access 1
    ps7_init
    ps7_post_config

    puts "== download enhanced GEMM test ELF =="
    dow $elf_file

    puts "== run enhanced GEMM test application =="
    configparams force-mem-access 0
    con

    puts "== wait for UART output =="
    after $run_wait_ms

    puts "== stop A9 #0 before next download =="
    targets -set -nocase -filter {name =~ "*A9*#0"}
    stop_a9_or_continue
}

puts ""
puts "== repeat script done: check UART output for PASS lines =="
