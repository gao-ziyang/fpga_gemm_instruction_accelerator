# Build a fresh Vivado board project for the 1024-capable explicit-banks HLS IP.
#
# Run from Vivado 2020.2:
#   source C:/Transformer/gzy_gemm_accel/scripts/vivado/build_accel_axi_1024_explicit_banks.tcl

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]

set project_name "accel_axi_1024_explicit_banks"
set project_dir  [file join $root_dir "vivado_board" $project_name]
set project_xpr  [file join $project_dir "${project_name}.xpr"]

set ip_repo_dir [file join $root_dir "vitis_hls_project" "gemmscheduleronly_baseline2_tile14_axi1024_explicit_banks" "solution1" "impl" "ip"]
set component_xml [file join $ip_repo_dir "component.xml"]

set bd_tcl [file join $root_dir "vivado_board" "accel_axi_112" "accel_axi_112.gen" "sources_1" "bd" "design_1" "hw_handoff" "design_1_bd.tcl"]

if {![file exists $component_xml]} {
    error "Missing HLS IP component.xml: $component_xml. Run hls/scripts/run_gemmscheduleronly_baseline2_tile14_axi1024_explicit_banks.tcl first."
}

if {![file exists $bd_tcl]} {
    error "Missing reference BD Tcl: $bd_tcl. The 112 Vivado project must exist first."
}

if {[file exists $project_xpr]} {
    error "Project already exists: $project_xpr. Move or remove it before rebuilding."
}

file mkdir [file dirname $project_dir]

create_project $project_name $project_dir -part xc7z020clg400-2
set_property ip_repo_paths [list $ip_repo_dir] [current_project]
update_ip_catalog

puts "INFO: using HLS IP repo: $ip_repo_dir"
puts "INFO: sourcing reference block design: $bd_tcl"
source $bd_tcl

set bd_files [get_files -quiet *design_1.bd]
if {[llength $bd_files] == 0} {
    error "Could not locate design_1.bd after sourcing BD Tcl."
}
set bd_file [lindex $bd_files 0]

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
