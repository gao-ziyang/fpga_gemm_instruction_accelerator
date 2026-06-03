# Board-facing AXI top for the larger GEMM validation:
#   MAX/depth = 1024, TILE=14, BLOCK=112.
#   GZY_ACCEL_FULL_ONLY stays 0, so boundary blocks such as 1024 are supported.
#
# C-sim keeps BENCH at 112 to avoid a very slow 1024^3 software simulation.
# The generated hardware still uses MAX_N/K/M=1024 and m_axi depth=1024^2.

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir "../.."]]
set src_dir    [file join $root_dir "hls/src"]
set tb_dir     [file join $root_dir "hls/tb"]
set proj_parent [file join $root_dir "vitis_hls_project"]

file mkdir $proj_parent
cd $proj_parent

set project_name "accel_axi_o1_1024"
set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=14 -DGZY_GEMM_BLOCK_M=14 -DGZY_ACCEL_BLOCK_N=112 -DGZY_ACCEL_BLOCK_K=112 -DGZY_ACCEL_BLOCK_M=112 -DGZY_ACCEL_LOAD_AB_PARALLEL=1 -DGZY_ACCEL_LOCAL_ROW_UNROLL=1 -DGZY_ACCEL_LOCAL_AB_PARALLEL=0 -DGZY_ACCEL_LOCAL_DOUBLE_BUFFER=0 -DGZY_ACCEL_DATAFLOW_BLOCK_OVERLAP=0 -DGZY_ACCEL_FULL_BLOCK_FAST=0 -DGZY_ACCEL_FULL_ONLY=0 -DGZY_ACCEL_MAX_N=1024 -DGZY_ACCEL_MAX_K=1024 -DGZY_ACCEL_MAX_M=1024 -DGZY_ACCEL_BENCH_N=112 -DGZY_ACCEL_BENCH_K=112 -DGZY_ACCEL_BENCH_M=112 -DGZY_ACCEL_MAX_INSTR=4" \
    $src_dir $tb_dir]

open_project -reset $project_name
set_top accelerator_top_axi

add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_instruction.cpp"]
add_files -cflags $cflags [file join $src_dir "accelerator_top_axi.cpp"]
add_files -tb -cflags $cflags [file join $tb_dir "tb_accelerator_top_axi.cpp"]

open_solution -reset "solution1" -flow_target vivado
set_part {xc7z020clg400-2}
create_clock -period 10 -name default

csim_design
csynth_design
set export_rc [catch {export_design -format ip_catalog} export_result]
if {$export_result ne ""} {
    puts $export_result
}
if {$export_rc != 0} {
    puts "WARNING: export_design returned an error; checking whether the Vivado 2020.2 core_revision workaround can recover IP packaging."
}

# Vivado/Vitis HLS 2020.2 can generate a core_revision like 2605312313 on
# newer dates, which overflows the old IP packager. If packaging failed,
# rewrite only that generated revision field and rerun the packager.
set ip_dir [file join $proj_parent $project_name "solution1" "impl" "ip"]
set component_xml [file join $ip_dir "component.xml"]
set ip_pack_tcl [file join $ip_dir "run_ippack.tcl"]
if {![file exists $component_xml] && [file exists $ip_pack_tcl]} {
    puts "INFO: component.xml missing after export_design; applying Vivado 2020.2 core_revision workaround."

    set fp [open $ip_pack_tcl r]
    set ip_pack_data [read $fp]
    close $fp

    regsub {set Revision[ \t]+"[0-9]+"} $ip_pack_data {set Revision    "1"} ip_pack_data

    set fp [open $ip_pack_tcl w]
    puts -nonewline $fp $ip_pack_data
    close $fp

    set old_dir [pwd]
    cd $ip_dir

    set vivado_cmd "vivado"
    set vivado_bat "C:/Xilinx/Vivado/2020.2/bin/vivado.bat"
    if {[file exists $vivado_bat]} {
        set vivado_cmd $vivado_bat
    }

    if {[catch {exec $vivado_cmd -notrace -mode batch -source $ip_pack_tcl} pack_result]} {
        puts "WARNING: IP repack workaround failed:"
        puts $pack_result
        if {$export_rc != 0} {
            error "export_design failed and IP repack workaround also failed."
        }
    } else {
        puts $pack_result
        puts "INFO: IP repack workaround finished."
        set export_rc 0
    }

    cd $old_dir
}

if {$export_rc != 0} {
    error "export_design failed and no IP repack workaround was applied."
}

set driver_makefile [file join $ip_dir "drivers" "accelerator_top_axi_v1_0" "src" "Makefile"]
if {[file exists $driver_makefile]} {
    puts "INFO: applying Windows CMD workaround to generated HLS driver Makefile."

    set fp [open $driver_makefile r]
    set makefile_data [read $fp]
    close $fp

    set makefile_data [string map [list "\t#echo" "\t@echo"] $makefile_data]

    set fp [open $driver_makefile w]
    puts -nonewline $fp $makefile_data
    close $fp
}

close_project

if {![info exists ::GZY_NO_EXIT]} {
    exit
}
