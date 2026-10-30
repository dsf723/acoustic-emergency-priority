# =============================================================================
# 综合、实现、生成比特流，并导出 SDK 用的硬件描述文件 build/system_wrapper.hdf
# 用法：vivado -mode batch -source build.tcl
#       （或在已打开工程的 Tcl Console 中 source build.tcl）
# =============================================================================

set script_dir [file dirname [file normalize [info script]]]
set root       [file normalize $script_dir/..]
set proj_dir   $root/build

if {[catch {current_project}]} {
    open_project $proj_dir/sound_loc.xpr
}

# 检查引脚是否已分配
set unplaced {}
foreach p {mic_sck mic_ws mic_d[0] mic_d[1] mic_d[2] mic_d[3] led_ck led_da oled_iic_scl_io oled_iic_sda_io sys_clk hdmi_hpd tmds_clk_p tmds_data_p[0] tmds_data_p[1] tmds_data_p[2] tl_out[0] tl_out[1] tl_out[2] tl_out[3] tl_out[4] tl_out[5] tl_out[6] tl_out[7] tl_out[8] tl_out[9] tl_out[10] tl_out[11] tl_out[12] tl_out[13] tl_out[14]} {
    set found 0
    foreach f [get_files -of_objects [get_filesets constrs_1]] {
        set fp [open $f r]
        set txt [read $fp]
        close $fp
        foreach line [split $txt "\n"] {
            set line [string trim $line]
            if {[string match "set_property*PACKAGE_PIN*" $line] && [string first $p $line] >= 0} {
                set found 1
            }
        }
    }
    if {!$found} { lappend unplaced $p }
}
if {[llength $unplaced] > 0} {
    puts "ERROR: no PACKAGE_PIN for ports: $unplaced  (edit constr/sound_loc.xdc first)"
    return -code error
}

reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "ERROR: synthesis failed"
    return -code error
}

launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "ERROR: implementation failed"
    return -code error
}

open_run impl_1
report_timing_summary -file $proj_dir/timing_summary.rpt
set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
puts "INFO: worst setup slack WNS = $wns ns"

# 2018.3：impl 生成的 .sysdef 即包含比特流的硬件描述文件，SDK 直接使用 .hdf
file copy -force $proj_dir/sound_loc.runs/impl_1/system_wrapper.sysdef $proj_dir/system_wrapper.hdf
puts "INFO: exported $proj_dir/system_wrapper.hdf"
