# =============================================================================
# 生成上电自启动镜像并烧写 QSPI Flash（脱离电脑运行）
# 用法：开发板上电、JTAG 连好、拨码开关为 JTAG（两位都 ON）时执行
#   D:\Xilinx\SDK\2018.3\bin\xsct.bat make_boot.tcl          生成 BOOT.bin 并烧写 QSPI
#   D:\Xilinx\SDK\2018.3\bin\xsct.bat make_boot.tcl noflash  只生成 BOOT.bin（可拷到 SD 卡根目录用 SD 启动）
# 烧写完成后断电，拨码开关改为 QSPI 启动（开关1 ON、开关2 OFF），再上电即自动运行。
# 需要先运行过 create_sdk.tcl（或 rebuild_app.tcl）得到最新 ELF。
# =============================================================================

set script_dir [file dirname [file normalize [info script]]]
set ws   $script_dir/workspace
set hw   $ws/hw_platform
set elf  $ws/sound_loc/Debug/sound_loc.elf
set out  $script_dir/boot
set noflash [expr {[llength $argv] > 0 && [lindex $argv 0] eq "noflash"}]

setws $ws
if {![file exists $ws/fsbl]} {
    createapp -name fsbl -hwproject hw_platform -proc ps7_cortexa9_0 \
              -os standalone -lang C -app {Zynq FSBL}
}
projects -build -type bsp -name fsbl_bsp
projects -build -type app -name fsbl
set fsbl $ws/fsbl/Debug/fsbl.elf
foreach f [list $fsbl $hw/system_wrapper.bit $elf] {
    if {![file exists $f]} {
        puts "ERROR: $f not found"
        exit 1
    }
}

file mkdir $out
set bif [open $out/boot.bif w]
puts $bif "the_ROM_image:"
puts $bif "\{"
puts $bif "    \[bootloader\]$fsbl"
puts $bif "    $hw/system_wrapper.bit"
puts $bif "    $elf"
puts $bif "\}"
close $bif
exec bootgen -image $out/boot.bif -arch zynq -o $out/BOOT.bin -w on
puts "INFO: $out/BOOT.bin generated"

if {$noflash} {
    exit 0
}
puts "INFO: programming QSPI flash (about 1-2 minutes) ..."
puts [exec program_flash -f $out/BOOT.bin -offset 0 -flash_type qspi_single \
                         -fsbl $fsbl]
puts "INFO: done. Power off, set BOOT switch to QSPI (SW1 ON, SW2 OFF), power on."
