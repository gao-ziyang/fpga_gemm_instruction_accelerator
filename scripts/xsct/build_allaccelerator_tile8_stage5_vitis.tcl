# Create/build Vitis platform and standalone PS application for all-accelerator TILE8 stage5.
#
# Run:
#   C:/Xilinx/Vitis/2020.2/bin/xsct.bat C:/Transformer/gzy_gemm_accel/scripts/xsct/build_allaccelerator_tile8_stage5_vitis.tcl

set root_dir "C:/Transformer/gzy_gemm_accel"
set ws_dir   "$root_dir/vitis_ws_allaccelerator_tile8_stage5"
set xsa_file "$root_dir/vivado_board/allaccelerator_baseline_tile8_stage5/export/allaccelerator_baseline_tile8_stage5.xsa"
set app_src  "$root_dir/ps_apps/allaccelerator_tile8_stage5_test/helloworld.c"

set platform_name "allaccelerator_tile8_stage5"
set app_name      "allaccelerator_tile8_stage5_test"

if {![file exists $xsa_file]} {
    error "Missing XSA: $xsa_file. Run scripts/vivado/build_allaccelerator_baseline_tile8_stage5.tcl first."
}

if {![file exists $app_src]} {
    error "Missing PS app source: $app_src"
}

file delete -force "$ws_dir/$platform_name" "$ws_dir/$app_name" "$ws_dir/${app_name}_system"
file mkdir $ws_dir

setws $ws_dir

platform create -name $platform_name \
    -hw $xsa_file \
    -proc ps7_cortexa9_0 \
    -os standalone \
    -fsbl-target psu_cortexa53_0 \
    -out $ws_dir

platform write
platform generate -domains
platform active $platform_name
platform generate

app create -name $app_name \
    -platform $platform_name \
    -domain standalone_domain \
    -template {Hello World} \
    -lang C

file copy -force $app_src "$ws_dir/$app_name/src/helloworld.c"

app build -name $app_name

set elf_file "$ws_dir/$app_name/Debug/${app_name}.elf"
if {![file exists $elf_file]} {
    error "ELF was not generated: $elf_file"
}

puts "INFO: built ELF: $elf_file"
