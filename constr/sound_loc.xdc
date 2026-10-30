# =============================================================================
# 启明星 ZYNQ7020 + Sipeed 6+1 麦克风阵列 + 0.96 寸 I2C OLED 约束
#
# 所有外设接底板扩展口 J4（EXT_IO，2x20 排针）。J4 的 1~26 脚为 Bank13 IO（3.3V，
# 仅 7020 核心板有），引脚号依据《启明星ZYNQ底板原理图_V2.3.2》第 2 页 J1/J4。
#
#   J4 脚  网络名        FPGA   信号                接到
#   ----   -----------  -----  ------------------  ------------------------
#    1     B13_L6_N     V5     mic_sck             阵列 8  MIC_CK
#    3     B13_L19_N    U5     mic_ws              阵列 7  MIC_WS
#    5     B13_L11_N    V7     mic_d[0]            阵列 3  MIC_D0
#    7     B13_L15_N    W8     mic_d[1]            阵列 4  MIC_D1
#    9     B13_L17_P    U9     mic_d[2]            阵列 5  MIC_D2
#   11     B13_L12_N    U10    mic_d[3]            阵列 6  MIC_D3
#    4     B13_L19_P    T5     led_ck              阵列 9  LED_CK
#    6     B13_L11_P    U7     led_da              阵列 10 LED_DA
#    8     B13_L15_P    V8     oled_iic_scl_io     OLED SCL
#   10     B13_L17_N    U8     oled_iic_sda_io     OLED SDA
#   37/39  GND                                     阵列 2 GND、OLED GND
#   38     +3.3V                                   OLED VCC
#   40     +5V                                     阵列 1 VIN（按商家标注 5V 供电）
#
# 注意：J4.2（B34_L1_P/T11）与摄像头 SCCB_SDA 共用，这里没有使用。
# =============================================================================

set_property PACKAGE_PIN V5  [get_ports mic_sck]
set_property PACKAGE_PIN U5  [get_ports mic_ws]
set_property PACKAGE_PIN V7  [get_ports {mic_d[0]}]
set_property PACKAGE_PIN W8  [get_ports {mic_d[1]}]
set_property PACKAGE_PIN U9  [get_ports {mic_d[2]}]
set_property PACKAGE_PIN U10 [get_ports {mic_d[3]}]
set_property PACKAGE_PIN T5  [get_ports led_ck]
set_property PACKAGE_PIN U7  [get_ports led_da]
set_property PACKAGE_PIN V8  [get_ports oled_iic_scl_io]
set_property PACKAGE_PIN U8  [get_ports oled_iic_sda_io]

set_property IOSTANDARD LVCMOS33 [get_ports {mic_sck mic_ws {mic_d[*]} led_ck led_da}]
set_property IOSTANDARD LVCMOS33 [get_ports {oled_iic_scl_io oled_iic_sda_io}]

# I2C 开漏总线：OLED 模块一般自带上拉电阻，这里再加内部上拉作为补充
set_property PULLUP true [get_ports {oled_iic_scl_io oled_iic_sda_io}]

# 中心麦所在数据线有一个空声道（高阻），加下拉保持确定电平
set_property PULLDOWN true [get_ports {mic_d[*]}]

# 杜邦线连接，降低驱动强度和翻转速率以减小振铃
set_property DRIVE 8 [get_ports {mic_sck mic_ws led_ck led_da}]
set_property SLEW SLOW [get_ports {mic_sck mic_ws led_ck led_da}]

# 麦克风数据经两级同步器采样（逻辑上保证约 100ns 余量），
# SCK/WS/LED 为低速寄存器输出，均不做 IO 时序分析
set_false_path -from [get_ports {mic_d[*]}]
set_false_path -to [get_ports {mic_sck mic_ws led_ck led_da}]
# I2C 为 400kHz 低速总线，同样不做 IO 时序分析
set_false_path -from [get_ports {oled_iic_scl_io oled_iic_sda_io}]
set_false_path -to [get_ports {oled_iic_scl_io oled_iic_sda_io}]

# =============================================================================
# HDMI（底板 HDMI 口，Bank35，引脚依据资料盘 PHOSPHOR_ZYNQ_IO.xdc 与底板原理图第 5 页）
#   sys_clk：PL 板载 50MHz 晶振，作视频时钟源（与 PS 的 FCLK0 异步）
#   TMDS 只约束 P 端，N 端由差分对自动确定
# =============================================================================
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
create_clock -period 20.000 -name sys_clk [get_ports sys_clk]

set_property -dict {PACKAGE_PIN L19 IOSTANDARD LVCMOS33} [get_ports hdmi_hpd]
set_property PACKAGE_PIN J18 [get_ports tmds_clk_p]
set_property PACKAGE_PIN G19 [get_ports {tmds_data_p[0]}]
set_property PACKAGE_PIN K19 [get_ports {tmds_data_p[1]}]
set_property PACKAGE_PIN J20 [get_ports {tmds_data_p[2]}]
set_property IOSTANDARD TMDS_33 [get_ports {tmds_clk_p tmds_clk_n {tmds_data_p[*]} {tmds_data_n[*]}}]

# 视频时钟（sys_clk 及 MMCM 派生时钟）与 PS 的 AXI 时钟互不相关，跨域只经过双口 BRAM 和同步器
set_clock_groups -asynchronous \
    -group [get_clocks -include_generated_clocks sys_clk] \
    -group [get_clocks -include_generated_clocks clk_fpga_0]
set_false_path -from [get_ports hdmi_hpd]

# =============================================================================
# 路口信号灯（应急车辆声学让行，2026-10-24），全部在 J4（Bank13，3.3V）
#   tl_out 位   J4 脚  网络         FPGA   接到
#   ---------   -----  -----------  -----  ----------------
#    0          12     B13_L12_P    T9     北口 红
#    1          14     B13_L22_P    V6     北口 黄
#    2          16     B13_L13_N    Y6     北口 绿
#    3          13     B13_L22_N    W6     南口 红
#    4          15     B13_L13_P    Y7     南口 黄
#    5          17     B13_L14_P    Y9     南口 绿
#    6          18     B13_L14_N    Y8     东口 红
#    7          20     B13_L16_N    W9     东口 黄
#    8          22     B13_L21_N    V10    东口 绿
#    9          19     B13_L16_P    W10    西口 红
#   10          21     B13_L21_P    V11    西口 黄
#   11          23     B13_L20_N    Y13    西口 绿
#   12          24     B13_L20_P    Y12    优先放行指示灯
#   13/14       25/26  B13_L18_N/P  Y11/W11 备用
# 灯模块的 GND 接 J4.37/39（与阵列、OLED 共地，可用面包板分出）。
# 高电平点亮；IO 最大 8mA，裸 LED 须串 330Ω~1kΩ 电阻（常见红绿灯模块已自带电阻）。
# J4.27~36 与摄像头接口共用，未使用。
# =============================================================================
set_property PACKAGE_PIN T9  [get_ports {tl_out[0]}]
set_property PACKAGE_PIN V6  [get_ports {tl_out[1]}]
set_property PACKAGE_PIN Y6  [get_ports {tl_out[2]}]
set_property PACKAGE_PIN W6  [get_ports {tl_out[3]}]
set_property PACKAGE_PIN Y7  [get_ports {tl_out[4]}]
set_property PACKAGE_PIN Y9  [get_ports {tl_out[5]}]
set_property PACKAGE_PIN Y8  [get_ports {tl_out[6]}]
set_property PACKAGE_PIN W9  [get_ports {tl_out[7]}]
set_property PACKAGE_PIN V10 [get_ports {tl_out[8]}]
set_property PACKAGE_PIN W10 [get_ports {tl_out[9]}]
set_property PACKAGE_PIN V11 [get_ports {tl_out[10]}]
set_property PACKAGE_PIN Y13 [get_ports {tl_out[11]}]
set_property PACKAGE_PIN Y12 [get_ports {tl_out[12]}]
set_property PACKAGE_PIN Y11 [get_ports {tl_out[13]}]
set_property PACKAGE_PIN W11 [get_ports {tl_out[14]}]
set_property IOSTANDARD LVCMOS33 [get_ports {tl_out[*]}]
set_property DRIVE 8 [get_ports {tl_out[*]}]
set_property SLEW SLOW [get_ports {tl_out[*]}]
set_false_path -to [get_ports {tl_out[*]}]
