# XSCT script for layer 2: AXI-Lite / IP register sanity test.
# Use after Program Device has loaded accel_axi_112.bit.
#
# In XSCT:
#   source C:/Transformer/gzy_gemm_accel/scripts/xsct_run_ip_reg_test.tcl

set psinit_file "C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/_ide/psinit/ps7_init.tcl"
set elf_file    "C:/Transformer/gzy_gemm_accel/vitis_ws/accel_axi_112_ip_reg_test/Debug/accel_axi_112_ip_reg_test.elf"

puts "== connect to hw_server =="
connect

puts "== list targets =="
puts [targets]

puts "== select ARM Cortex-A9 #0 =="
targets -set -nocase -filter {name =~ "*A9*#0"}

puts "== stop A9 #0 =="
stop

puts "== initialize PS/DDR =="
configparams force-mem-access 1
source $psinit_file
ps7_init
ps7_post_config

puts "== download ELF =="
dow $elf_file

puts "== run application =="
configparams force-mem-access 0
con

puts "== done: check UART output =="
