# =============================================================================
# 用 xsct 创建并编译 SDK 2018.3 工程（硬件平台 + BSP + 应用）
# 用法：D:\Xilinx\SDK\2018.3\bin\xsct.bat create_sdk.tcl
# 需要先运行 vivado/build.tcl 生成 build/system_wrapper.hdf
# 完成后可在 SDK 中 File -> Switch Workspace 打开 sw/workspace 继续调试与下载
# =============================================================================

set script_dir [file dirname [file normalize [info script]]]
set root       [file normalize $script_dir/..]
set hdf        $root/build/system_wrapper.hdf
set ws         $script_dir/workspace

if {![file exists $hdf]} {
    puts "ERROR: $hdf not found, run vivado/build.tcl first"
    exit 1
}

file delete -force $ws
setws $ws

createhw  -name hw_platform -hwspec $hdf
createapp -name sound_loc -hwproject hw_platform -proc ps7_cortexa9_0 \
          -os standalone -lang C -app {Empty Application}
importsources -name sound_loc -path $script_dir/src

configapp -app sound_loc -add libraries m
configapp -app sound_loc -set compiler-optimization {Optimize more (-O2)}

projects -build
puts "INFO: done, ELF: $ws/sound_loc/Debug/sound_loc.elf"
