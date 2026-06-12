# Build a fresh Vivado board project for the all-accelerator TILE8 stage5 HLS IP.
#
# Run from Vivado 2020.2:
#   source C:/Transformer/gzy_gemm_accel/scripts/vivado/build_allaccelerator_baseline_tile8_stage5.tcl

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]

set project_name "allaccelerator_baseline_tile8_stage5"
set project_dir  [file join $root_dir "vivado_board" $project_name]
set project_xpr  [file join $project_dir "${project_name}.xpr"]

set ip_repo_dir [file join $root_dir "vitis_hls_project" "allaccelerator_baseline_tile8_stage5" "solution1" "impl" "ip"]
set component_xml [file join $ip_repo_dir "component.xml"]

set bd_tcl [file join $root_dir "vivado_board" "accel_axi_112" "accel_axi_112.gen" "sources_1" "bd" "design_1" "hw_handoff" "design_1_bd.tcl"]

if {![file exists $component_xml]} {
    error "Missing HLS IP component.xml: $component_xml. Run hls/scripts/run_allaccelerator_baseline_tile8_stage5.tcl first."
}

if {![file exists $bd_tcl]} {
    error "Missing reference BD Tcl: $bd_tcl. The 112 Vivado project must exist first."
}

if {[file exists $project_dir]} {
    puts "INFO: removing previous generated Vivado project: $project_dir"
    file delete -force $project_dir
}

file mkdir [file dirname $project_dir]

create_project $project_name $project_dir -part xc7z020clg400-2
set_property ip_repo_paths [list $ip_repo_dir] [current_project]
update_ip_catalog

puts "INFO: using HLS IP repo: $ip_repo_dir"
puts "INFO: sourcing reference block design for PS/DDR/UART settings: $bd_tcl"

set source_rc [catch {source $bd_tcl} source_result]
if {$source_result ne ""} {
    puts $source_result
}

if {$source_rc != 0} {
    puts "INFO: reference BD Tcl stopped while connecting the old single m_axi_gmem port; reconnecting for split AXI bundles."
}

set bd_files [get_files -quiet *design_1.bd]
if {[llength $bd_files] == 0} {
    error "Could not locate design_1.bd after sourcing BD Tcl."
}
set bd_file [lindex $bd_files 0]
open_bd_design $bd_file

set hls_ip [get_bd_cells accelerator_top_axi_0]
set ps     [get_bd_cells processing_system7_0]
set mem_ic [get_bd_cells axi_mem_intercon]
set gp_ic  [get_bd_cells ps7_0_axi_periph]
set rst    [get_bd_cells rst_ps7_0_50M]

if {$hls_ip eq "" || $ps eq "" || $mem_ic eq "" || $gp_ic eq "" || $rst eq ""} {
    error "Reference BD did not create the expected PS/HLS/interconnect/reset cells."
}

set_property -dict [list CONFIG.NUM_SI {4} CONFIG.NUM_MI {1}] $mem_ic
set_property -dict [list CONFIG.NUM_MI {1}] $gp_ic

connect_bd_intf_net [get_bd_intf_ports DDR] [get_bd_intf_pins $ps/DDR]
connect_bd_intf_net [get_bd_intf_ports FIXED_IO] [get_bd_intf_pins $ps/FIXED_IO]

connect_bd_intf_net [get_bd_intf_pins $hls_ip/m_axi_instr_gmem] [get_bd_intf_pins $mem_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins $hls_ip/m_axi_a_gmem]     [get_bd_intf_pins $mem_ic/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins $hls_ip/m_axi_b_gmem]     [get_bd_intf_pins $mem_ic/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins $hls_ip/m_axi_c_gmem]     [get_bd_intf_pins $mem_ic/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins $mem_ic/M00_AXI]          [get_bd_intf_pins $ps/S_AXI_HP0]

connect_bd_intf_net [get_bd_intf_pins $ps/M_AXI_GP0]            [get_bd_intf_pins $gp_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins $gp_ic/M00_AXI]           [get_bd_intf_pins $hls_ip/s_axi_control]

set fclk [get_bd_pins $ps/FCLK_CLK0]
connect_bd_net $fclk \
    [get_bd_pins $hls_ip/ap_clk] \
    [get_bd_pins $mem_ic/ACLK] \
    [get_bd_pins $mem_ic/M00_ACLK] \
    [get_bd_pins $mem_ic/S00_ACLK] \
    [get_bd_pins $mem_ic/S01_ACLK] \
    [get_bd_pins $mem_ic/S02_ACLK] \
    [get_bd_pins $mem_ic/S03_ACLK] \
    [get_bd_pins $ps/M_AXI_GP0_ACLK] \
    [get_bd_pins $ps/S_AXI_HP0_ACLK] \
    [get_bd_pins $gp_ic/ACLK] \
    [get_bd_pins $gp_ic/M00_ACLK] \
    [get_bd_pins $gp_ic/S00_ACLK] \
    [get_bd_pins $rst/slowest_sync_clk]

connect_bd_net [get_bd_pins $ps/FCLK_RESET0_N] [get_bd_pins $rst/ext_reset_in]

set aresetn [get_bd_pins $rst/peripheral_aresetn]
connect_bd_net $aresetn \
    [get_bd_pins $hls_ip/ap_rst_n] \
    [get_bd_pins $mem_ic/ARESETN] \
    [get_bd_pins $mem_ic/M00_ARESETN] \
    [get_bd_pins $mem_ic/S00_ARESETN] \
    [get_bd_pins $mem_ic/S01_ARESETN] \
    [get_bd_pins $mem_ic/S02_ARESETN] \
    [get_bd_pins $mem_ic/S03_ARESETN] \
    [get_bd_pins $gp_ic/ARESETN] \
    [get_bd_pins $gp_ic/M00_ARESETN] \
    [get_bd_pins $gp_ic/S00_ARESETN]

assign_bd_address -offset 0x00000000 -range 0x20000000 -target_address_space [get_bd_addr_spaces $hls_ip/Data_m_axi_instr_gmem] [get_bd_addr_segs $ps/S_AXI_HP0/HP0_DDR_LOWOCM] -force
assign_bd_address -offset 0x00000000 -range 0x20000000 -target_address_space [get_bd_addr_spaces $hls_ip/Data_m_axi_a_gmem]     [get_bd_addr_segs $ps/S_AXI_HP0/HP0_DDR_LOWOCM] -force
assign_bd_address -offset 0x00000000 -range 0x20000000 -target_address_space [get_bd_addr_spaces $hls_ip/Data_m_axi_b_gmem]     [get_bd_addr_segs $ps/S_AXI_HP0/HP0_DDR_LOWOCM] -force
assign_bd_address -offset 0x00000000 -range 0x20000000 -target_address_space [get_bd_addr_spaces $hls_ip/Data_m_axi_c_gmem]     [get_bd_addr_segs $ps/S_AXI_HP0/HP0_DDR_LOWOCM] -force
assign_bd_address -offset 0x40000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces $ps/Data] [get_bd_addr_segs $hls_ip/s_axi_control/Reg] -force

validate_bd_design
save_bd_design

generate_target all $bd_file
make_wrapper -files $bd_file -top

set wrapper_file [file join $project_dir "${project_name}.gen" "sources_1" "bd" "design_1" "hdl" "design_1_wrapper.v"]
if {![file exists $wrapper_file]} {
    error "Could not locate generated wrapper: $wrapper_file"
}
add_files -norecurse $wrapper_file
update_compile_order -fileset sources_1

set jobs 4
if {[info exists ::GZY_VIVADO_JOBS]} {
    set jobs $::GZY_VIVADO_JOBS
}

puts "INFO: launching synthesis/implementation/bitstream with jobs=$jobs"
launch_runs impl_1 -to_step write_bitstream -jobs $jobs
wait_on_run impl_1

set impl_status [get_property STATUS [get_runs impl_1]]
puts "INFO: impl_1 status: $impl_status"
if {[string first "Complete" $impl_status] < 0} {
    error "Implementation did not complete. Check Vivado run logs under $project_dir."
}

open_run impl_1

set export_dir [file join $project_dir "export"]
set report_dir [file join $project_dir "reports"]
file mkdir $export_dir
file mkdir $report_dir

report_utilization -file [file join $report_dir "utilization_impl.rpt"]
report_timing_summary -file [file join $report_dir "timing_summary_impl.rpt"]

set xsa_file [file join $export_dir "${project_name}.xsa"]
write_hw_platform -fixed -include_bit -force -file $xsa_file

puts "INFO: exported XSA: $xsa_file"
close_project
