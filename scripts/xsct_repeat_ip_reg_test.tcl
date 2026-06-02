# XSCT repeat test for layer 2: AXI-Lite / IP register sanity test.
# Use after Program Device has loaded accel_axi_112.bit.
#
# In XSCT:
#   source C:/Transformer/gzy_gemm_accel/scripts/xsct_repeat_ip_reg_test.tcl

set psinit_file "C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/_ide/psinit/ps7_init.tcl"
set elf_file    "C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/Debug/accel_axi_112_ip_reg_test.elf"
set repeat_count 5

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
    puts "========== manual XSCT run $i / $repeat_count =========="

    puts "== select ARM Cortex-A9 #0 =="
    targets -set -nocase -filter {name =~ "*A9*#0"}

    puts "== stop A9 #0 =="
    stop_a9_or_continue

    puts "== initialize PS/DDR =="
    configparams force-mem-access 1
    ps7_init
    ps7_post_config

    puts "== download ELF =="
    dow $elf_file

    puts "== run application =="
    configparams force-mem-access 0
    con

    puts "== wait for UART output =="
    after 1500
}

puts ""
puts "== final targets after repeated manual XSCT runs =="
puts [targets]
puts "== done: check whether UART printed PASS for each run =="
