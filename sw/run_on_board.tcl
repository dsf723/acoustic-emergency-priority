# =============================================================================
# 一键下载运行：配置 FPGA（比特流）-> 初始化 PS（DDR/时钟/MIO）-> 下载 ELF -> 运行
# 用法：开发板上电、连好 JTAG 后执行
#   D:\Xilinx\SDK\2018.3\bin\xsct.bat run_on_board.tcl
# 需要先运行过 create_sdk.tcl（生成 workspace 中的比特流、ps7_init.tcl 与 ELF）
# =============================================================================

set script_dir [file dirname [file normalize [info script]]]
set hw  $script_dir/workspace/hw_platform
set elf $script_dir/workspace/sound_loc/Debug/sound_loc.elf

foreach f [list $hw/system_wrapper.bit $hw/ps7_init.tcl $elf] {
    if {![file exists $f]} {
        puts "ERROR: $f not found, run create_sdk.tcl first"
        exit 1
    }
}

connect
puts "INFO: JTAG targets:"
puts [targets]

targets -set -nocase -filter {name =~ "APU*"}
rst -system
after 1000

puts "INFO: programming FPGA ..."
fpga -file $hw/system_wrapper.bit

targets -set -nocase -filter {name =~ "APU*"}
source $hw/ps7_init.tcl
ps7_init
ps7_post_config

targets -set -nocase -filter {name =~ "*A9*#0"}
puts "INFO: downloading $elf ..."
dow $elf
con
puts "INFO: running. Open the serial terminal (115200 8N1) to see output."
