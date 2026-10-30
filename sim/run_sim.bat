@echo off
rem 用 Vivado xsim 运行 mic_array_i2s_rx 仿真；按需修改 Vivado 安装路径
set VIVADO_BIN=D:\Xilinx\Vivado\2018.3\bin
cd /d %~dp0
if not exist work mkdir work
cd work
call %VIVADO_BIN%\xvlog.bat ..\..\rtl\mic_array_i2s_rx.v ..\tb_mic_array_i2s_rx.v || exit /b 1
call %VIVADO_BIN%\xelab.bat -debug typical tb_mic_array_i2s_rx -s tb_sim || exit /b 1
call %VIVADO_BIN%\xsim.bat tb_sim -runall
