# =============================================================================
# 创建 Vivado 2018.3 工程与 Block Design
#
# 用法（任选其一）：
#   1) Vivado Tcl Console：cd <仓库目录>/vivado ; source create_project.tcl
#   2) 命令行：vivado -mode batch -source create_project.tcl
#
# PS 配置（DDR 型号、UART/SD/QSPI 的 MIO 等）与具体开发板相关：
#   把正点原子启明星例程中 ZYNQ7 IP 的配置导出为预设文件（双击 ZYNQ7 IP ->
#   Presets -> Save Configuration），保存为本目录下的 ps7_preset.tcl，脚本会自动应用。
#   没有该文件时 PS 使用默认配置，需要在 Block Design 里手动按启明星手册配置 DDR 和 UART。
# =============================================================================

set script_dir [file dirname [file normalize [info script]]]
set root       [file normalize $script_dir/..]
set proj_name  sound_loc
set proj_dir   $root/build
set ps_preset  $script_dir/ps7_preset.tcl

# 每个 DMA 包的帧数，必须与 sw/src/doa_config.h 中 DOA_FRAMES_PER_PKT 一致
set frames_per_pkt 1024

create_project $proj_name $proj_dir -part xc7z020clg400-2 -force
set_property target_language Verilog [current_project]
add_files -norecurse $root/rtl/mic_array_i2s_rx.v
add_files -norecurse $root/rtl/hdmi_fb.v
add_files -fileset constrs_1 -norecurse $root/constr/sound_loc.xdc
# 放在 IP 约束之后处理：其中的时钟分组引用了 PS7 IP 约束创建的 clk_fpga_0
set_property PROCESSING_ORDER LATE [get_files $root/constr/sound_loc.xdc]
# 综合时 PS7 IP 为独立综合，clk_fpga_0 尚不存在；引脚与时钟约束只在实现阶段使用
set_property USED_IN_SYNTHESIS false [get_files $root/constr/sound_loc.xdc]
update_compile_order -fileset sources_1

create_bd_design system

# ---------------- ZYNQ7 PS ----------------
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7]
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "0" Master "Disable" Slave "Disable"} $ps

if {[file exists $ps_preset]} {
    puts "INFO: applying PS preset $ps_preset"
    source $ps_preset
    set_property -dict [apply_preset $ps] $ps
} else {
    puts "WARNING: $ps_preset not found. PS uses default settings - configure DDR and UART for your board manually!"
}

set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} \
    CONFIG.PCW_EN_CLK0_PORT {1} \
    CONFIG.PCW_EN_RST0_PORT {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {50} \
    CONFIG.PCW_I2C0_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_I2C0_I2C0_IO {EMIO} \
    CONFIG.PCW_I2C1_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_I2C1_I2C1_IO {EMIO} \
] $ps

# ---------------- 麦克风阵列采集（RTL 模块引用） ----------------
set mic [create_bd_cell -type module -reference mic_array_i2s_rx mic_rx]
set_property -dict [list CONFIG.SCK_HALF_DIV {8} CONFIG.FRAMES_PER_PKT $frames_per_pkt] $mic

# ---------------- AXI DMA（仅 S2MM，简单模式） ----------------
set dma [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 axi_dma_0]
set_property -dict [list \
    CONFIG.c_include_sg {0} \
    CONFIG.c_include_mm2s {0} \
    CONFIG.c_include_s2mm {1} \
    CONFIG.c_sg_length_width {23} \
    CONFIG.c_s2mm_burst_size {16} \
    CONFIG.c_m_axi_s2mm_data_width {32} \
] $dma

# ---------------- AXI GPIO ----------------
# 状态：通道 1 = frame_cnt（32bit 输入），通道 2 = overrun（1bit 输入）
set gst [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_status]
set_property -dict [list \
    CONFIG.C_ALL_INPUTS {1} CONFIG.C_GPIO_WIDTH {32} \
    CONFIG.C_IS_DUAL {1} CONFIG.C_ALL_INPUTS_2 {1} CONFIG.C_GPIO2_WIDTH {1} \
] $gst

# 灯环：bit0 = LED_CK，bit1 = LED_DA
set gled [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_led]
set_property -dict [list CONFIG.C_ALL_OUTPUTS {1} CONFIG.C_GPIO_WIDTH {2}] $gled

# Traffic lights (emergency-vehicle acoustic priority): 13 outputs on J4 pins 12..24
#   bits 0-2 north R/Y/G, 3-5 south, 6-8 east, 9-11 west, 12 priority indicator
#   (pin table in constr/sound_loc.xdc; comment kept ASCII on purpose, see HANDOFF section 27)
set gtl [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_tl]
set_property -dict [list CONFIG.C_ALL_OUTPUTS {1} CONFIG.C_GPIO_WIDTH {13} CONFIG.C_DOUT_DEFAULT {0x00000000}] $gtl

# East/west OLED buses: software I2C on a tri-state GPIO (the PS has only two I2C controllers)
#   bit 0 east SCL, 1 east SDA, 2 west SCL, 3 west SDA; ports oled_sw_tri_io[3:0]
set goled [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_oled]
set_property -dict [list CONFIG.C_GPIO_WIDTH {4} CONFIG.C_ALL_OUTPUTS {0} CONFIG.C_ALL_INPUTS {0}] $goled

# ---------------- HDMI 帧缓冲显示（RTL 模块引用，640x480，PS 经 AXI4-Lite 写像素） ----------------
set hdmi [create_bd_cell -type module -reference hdmi_fb hdmi]

# ---------------- AXI 连接（自动生成互联与复位） ----------------
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} Master {/ps7/M_AXI_GP0} Slave {/axi_dma_0/S_AXI_LITE} intc_ip {New AXI Interconnect} master_apm {0}} \
    [get_bd_intf_pins axi_dma_0/S_AXI_LITE]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} Master {/ps7/M_AXI_GP0} Slave {/axi_gpio_status/S_AXI} intc_ip {New AXI Interconnect} master_apm {0}} \
    [get_bd_intf_pins axi_gpio_status/S_AXI]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} Master {/ps7/M_AXI_GP0} Slave {/axi_gpio_led/S_AXI} intc_ip {New AXI Interconnect} master_apm {0}} \
    [get_bd_intf_pins axi_gpio_led/S_AXI]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} Master {/ps7/M_AXI_GP0} Slave {/hdmi/s_axi} intc_ip {New AXI Interconnect} master_apm {0}} \
    [get_bd_intf_pins hdmi/s_axi]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} Master {/ps7/M_AXI_GP0} Slave {/axi_gpio_tl/S_AXI} intc_ip {New AXI Interconnect} master_apm {0}} \
    [get_bd_intf_pins axi_gpio_tl/S_AXI]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} Master {/ps7/M_AXI_GP0} Slave {/axi_gpio_oled/S_AXI} intc_ip {New AXI Interconnect} master_apm {0}} \
    [get_bd_intf_pins axi_gpio_oled/S_AXI]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} Master {/axi_dma_0/M_AXI_S2MM} Slave {/ps7/S_AXI_HP0} intc_ip {Auto} master_apm {0}} \
    [get_bd_intf_pins ps7/S_AXI_HP0]

set rst [get_bd_cells -filter {VLNV =~ "xilinx.com:ip:proc_sys_reset:*"}]

connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins mic_rx/clk]
connect_bd_net [get_bd_pins $rst/peripheral_aresetn] [get_bd_pins mic_rx/rst_n]
connect_bd_intf_net [get_bd_intf_pins mic_rx/m_axis] [get_bd_intf_pins axi_dma_0/S_AXIS_S2MM]
connect_bd_net [get_bd_pins mic_rx/frame_cnt] [get_bd_pins axi_gpio_status/gpio_io_i]
connect_bd_net [get_bd_pins mic_rx/overrun]   [get_bd_pins axi_gpio_status/gpio2_io_i]

# ---------------- 外部端口 ----------------
create_bd_port -dir O mic_sck
create_bd_port -dir O mic_ws
create_bd_port -dir I -from 3 -to 0 mic_d
create_bd_port -dir O led_ck
create_bd_port -dir O led_da
connect_bd_net [get_bd_ports mic_sck] [get_bd_pins mic_rx/mic_sck]
connect_bd_net [get_bd_ports mic_ws]  [get_bd_pins mic_rx/mic_ws]
connect_bd_net [get_bd_ports mic_d]   [get_bd_pins mic_rx/mic_d]

# OLED：PS I2C0 经 EMIO 引出，wrapper 中生成 oled_iic_scl_io / oled_iic_sda_io 双向端口
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 oled_iic
connect_bd_intf_net [get_bd_intf_pins ps7/IIC_0] [get_bd_intf_ports oled_iic]
# Second OLED bus (south display): PS I2C1 via EMIO, oled2_iic_scl_io / oled2_iic_sda_io
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 oled2_iic
connect_bd_intf_net [get_bd_intf_pins ps7/IIC_1] [get_bd_intf_ports oled2_iic]
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gpio_rtl:1.0 oled_sw
connect_bd_intf_net [get_bd_intf_pins axi_gpio_oled/GPIO] [get_bd_intf_ports oled_sw]

# HDMI：板载 50MHz 晶振（视频时钟源）、热插拔检测、TMDS 差分输出
create_bd_port -dir I -type clk sys_clk
set_property CONFIG.FREQ_HZ 50000000 [get_bd_ports sys_clk]
create_bd_port -dir I hdmi_hpd
create_bd_port -dir O tmds_clk_p
create_bd_port -dir O tmds_clk_n
create_bd_port -dir O -from 2 -to 0 tmds_data_p
create_bd_port -dir O -from 2 -to 0 tmds_data_n
connect_bd_net [get_bd_ports sys_clk]     [get_bd_pins hdmi/clk_in]
connect_bd_net [get_bd_ports hdmi_hpd]    [get_bd_pins hdmi/hdmi_hpd]
connect_bd_net [get_bd_ports tmds_clk_p]  [get_bd_pins hdmi/tmds_ck_p]
connect_bd_net [get_bd_ports tmds_clk_n]  [get_bd_pins hdmi/tmds_ck_n]
connect_bd_net [get_bd_ports tmds_data_p] [get_bd_pins hdmi/tmds_data_p]
connect_bd_net [get_bd_ports tmds_data_n] [get_bd_pins hdmi/tmds_data_n]

create_bd_port -dir O -from 12 -to 0 tl_out
connect_bd_net [get_bd_ports tl_out] [get_bd_pins axi_gpio_tl/gpio_io_o]

foreach {name bit} {led_ck 0 led_da 1} {
    set sl [create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 slice_$name]
    set_property -dict [list CONFIG.DIN_WIDTH {2} CONFIG.DIN_FROM $bit CONFIG.DIN_TO $bit] $sl
    connect_bd_net [get_bd_pins axi_gpio_led/gpio_io_o] [get_bd_pins $sl/Din]
    connect_bd_net [get_bd_pins $sl/Dout] [get_bd_ports $name]
}

assign_bd_address
# HDMI 帧缓冲固定在 0x43C00000，256KB（软件 hdmi_ui.c 用 xparameters 中的基地址）
foreach seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces ps7/Data] -filter {NAME =~ "*hdmi*"}] {
    set_property range 256K $seg
    set_property offset 0x43C00000 $seg
}
regenerate_bd_layout
validate_bd_design
save_bd_design

# ---------------- 顶层 wrapper ----------------
set bd_file [get_files system.bd]
generate_target all $bd_file
set wrapper [make_wrapper -files $bd_file -top]
add_files -norecurse $wrapper
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1

puts "INFO: project created: $proj_dir/$proj_name.xpr"
