# Build the already-created all-accelerator TILE8 stage5 standalone app.
#
# This is useful after the full create script has produced the platform/app but
# Vitis exits before compiling the application.

set ws_dir "C:/Transformer/gzy_gemm_accel/vitis_ws_allaccelerator_tile8_stage5"
set app_name "allaccelerator_tile8_stage5_test"
set elf_file "$ws_dir/$app_name/Debug/${app_name}.elf"

setws $ws_dir
puts "INFO: workspace=[getws]"
puts "INFO: applications:"
puts [app list]

app build -name $app_name

if {![file exists $elf_file]} {
    error "ELF was not generated: $elf_file"
}

puts "INFO: built ELF: $elf_file"
