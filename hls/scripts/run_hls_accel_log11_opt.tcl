# Run the log11 non-AXI accelerator optimization checkpoints.

proc run_accel_case {project_name mode tile block loadab row_unroll max_dim bench_dim do_cosim} {
    set script_dir [file normalize [file dirname [info script]]]
    set root_dir   [file normalize [file join $script_dir "../.."]]
    set src_dir    [file join $root_dir "hls/src"]
    set tb_dir     [file join $root_dir "hls/tb"]
    set proj_parent [file join $root_dir "vitis_hls_project"]

    file mkdir $proj_parent
    cd $proj_parent

    set cflags [format "-I%s -I%s -DGZY_GEMM_TILE=%d -DGZY_GEMM_BLOCK_M=%d -DGZY_ACCEL_BLOCK_N=%d -DGZY_ACCEL_BLOCK_K=%d -DGZY_ACCEL_BLOCK_M=%d -DGZY_ACCEL_LOAD_AB_PARALLEL=%d -DGZY_ACCEL_LOCAL_ROW_UNROLL=%d -DGZY_ACCEL_MAX_N=%d -DGZY_ACCEL_MAX_K=%d -DGZY_ACCEL_MAX_M=%d -DGZY_ACCEL_BENCH_N=%d -DGZY_ACCEL_BENCH_K=%d -DGZY_ACCEL_BENCH_M=%d" \
        $src_dir $tb_dir $tile $tile $block $block $block $loadab $row_unroll $max_dim $max_dim $max_dim $bench_dim $bench_dim $bench_dim]

    open_project -reset $project_name

    if {$mode eq "scheduler"} {
        set_top gemm_scheduler_top
        add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
        add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
        add_files -cflags $cflags [file join $src_dir "gemm_scheduler_top.cpp"]
        add_files -tb -cflags $cflags [file join $tb_dir "tb_gemm_scheduler.cpp"]
    } elseif {$mode eq "decode"} {
        set_top instruction_decode_top
        add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
        add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
        add_files -cflags $cflags [file join $src_dir "accelerator_instruction.cpp"]
        add_files -cflags $cflags [file join $src_dir "instruction_decode_top.cpp"]
        add_files -tb -cflags $cflags [file join $tb_dir "tb_instruction_decode.cpp"]
    } else {
        set_top accelerator_top
        add_files -cflags $cflags [file join $src_dir "gemm_core.cpp"]
        add_files -cflags $cflags [file join $src_dir "gemm_scheduler.cpp"]
        add_files -cflags $cflags [file join $src_dir "accelerator_instruction.cpp"]
        add_files -cflags $cflags [file join $src_dir "accelerator_top.cpp"]
        add_files -tb -cflags $cflags [file join $tb_dir "tb_accelerator_top.cpp"]
    }

    open_solution -reset "solution1" -flow_target vivado
    set_part {xc7z020clg400-2}
    create_clock -period 10 -name default

    csim_design
    csynth_design
    if {$do_cosim} {
        cosim_design -rtl verilog
    }

    close_project
}

run_accel_case "accel_log11_o0_tile14_serial_128"       "scheduler" 14 112 0 1 128 128 1
run_accel_case "accel_log11_o1_tile14_loadab_128"       "scheduler" 14 112 1 1 128 128 1
run_accel_case "accel_log11_o2_tile14_loadab_bank2_128" "scheduler" 14 112 1 2 128 128 1
run_accel_case "accel_log11_v2_decode64_bank2_128"      "decode"    14 112 1 2 128 128 1
run_accel_case "accel_log11_v3_top64_bank2_128"         "top"       14 112 1 2 128 128 1

run_accel_case "accel_log11_v1_tile14_bank2_1024"       "scheduler" 14 112 1 2 1024 1024 0
run_accel_case "accel_log11_v3_top64_bank2_1024"        "top"       14 112 1 2 1024 1024 0

exit
