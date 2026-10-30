# =============================================================================
# 正点原子启明星 ZYNQ7020 的 PS 配置预设
# 数据来源：资料盘 4_Source_Code/2_Embedded_Vitis/ZYNQ_Vitis_7020.rar 中
#           1_hello_world 与 19_axi_dma_loop 工程的 system.bd（processing_system7_0 参数）
# 只保留与开发板硬件相关的设置（DDR、晶振、MIO Bank 电压、UART、QSPI、SD），
# AXI 接口、PL 时钟、I2C0 等由 create_project.tcl 在应用预设之后另行设置。
# =============================================================================

proc getPresetInfo {} {
    return [dict create name {ALIENTEK_QIMINGXING_7020} description {ALIENTEK QiMingXing ZYNQ7020 board}]
}

proc validate_preset {IPINST} {
    return true
}

proc apply_preset {IPINST} {
    return [dict create \
        CONFIG.PCW_PRESET_BANK0_VOLTAGE                {LVCMOS 3.3V} \
        CONFIG.PCW_PRESET_BANK1_VOLTAGE                {LVCMOS 1.8V} \
        CONFIG.PCW_CRYSTAL_PERIPHERAL_FREQMHZ          {33.333333} \
        CONFIG.PCW_APU_PERIPHERAL_FREQMHZ              {666.666666} \
        \
        CONFIG.PCW_UIPARAM_DDR_ENABLE                  {1} \
        CONFIG.PCW_UIPARAM_DDR_MEMORY_TYPE             {DDR 3 (Low Voltage)} \
        CONFIG.PCW_UIPARAM_DDR_PARTNO                  {MT41K256M16 RE-125} \
        CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH               {32 Bit} \
        CONFIG.PCW_UIPARAM_DDR_FREQ_MHZ                {533.333333} \
        CONFIG.PCW_UIPARAM_DDR_TRAIN_WRITE_LEVEL       {1} \
        CONFIG.PCW_UIPARAM_DDR_TRAIN_READ_GATE         {1} \
        CONFIG.PCW_UIPARAM_DDR_TRAIN_DATA_EYE          {1} \
        CONFIG.PCW_UIPARAM_DDR_USE_INTERNAL_VREF       {0} \
        CONFIG.PCW_UIPARAM_DDR_BOARD_DELAY0            {0.25} \
        CONFIG.PCW_UIPARAM_DDR_BOARD_DELAY1            {0.25} \
        CONFIG.PCW_UIPARAM_DDR_BOARD_DELAY2            {0.25} \
        CONFIG.PCW_UIPARAM_DDR_BOARD_DELAY3            {0.25} \
        CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_0      {0.0} \
        CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_1      {0.0} \
        CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_2      {0.0} \
        CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_3      {0.0} \
        \
        CONFIG.PCW_UART0_PERIPHERAL_ENABLE             {1} \
        CONFIG.PCW_UART0_UART0_IO                      {MIO 14 .. 15} \
        CONFIG.PCW_UART0_BAUD_RATE                     {115200} \
        CONFIG.PCW_UART1_PERIPHERAL_ENABLE             {0} \
        \
        CONFIG.PCW_QSPI_PERIPHERAL_ENABLE              {1} \
        CONFIG.PCW_QSPI_GRP_SINGLE_SS_ENABLE           {1} \
        CONFIG.PCW_SINGLE_QSPI_DATA_MODE               {x4} \
        \
        CONFIG.PCW_SD0_PERIPHERAL_ENABLE               {1} \
        CONFIG.PCW_SD0_SD0_IO                          {MIO 40 .. 45} \
        CONFIG.PCW_SD0_GRP_CD_ENABLE                   {1} \
        CONFIG.PCW_SD0_GRP_CD_IO                       {MIO 10} \
        \
        CONFIG.PCW_ENET0_PERIPHERAL_ENABLE             {0} \
        CONFIG.PCW_USB0_PERIPHERAL_ENABLE              {0} \
        CONFIG.PCW_GPIO_MIO_GPIO_ENABLE                {0} \
    ]
}
