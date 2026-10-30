@echo off
rem 用 Vivado xsim 运行 hdmi_fb 仿真（需要 unisim 库：MMCM / OSERDES / BUFG）
set VIVADO_BIN=D:\Xilinx\Vivado\2018.3\bin
cd /d %~dp0
if not exist work_hdmi mkdir work_hdmi
cd work_hdmi
call %VIVADO_BIN%\xvlog.bat ..\..\rtl\hdmi_fb.v ..\tb_hdmi_fb.v %VIVADO_BIN%\..\data\verilog\src\glbl.v || exit /b 1
call %VIVADO_BIN%\xelab.bat -debug typical -L unisims_ver -L secureip tb_hdmi_fb glbl -s tb_hdmi || exit /b 1
call %VIVADO_BIN%\xsim.bat tb_hdmi -runall
