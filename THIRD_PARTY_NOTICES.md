# 第三方内容声明

除下列内容外，本仓库代码按根目录 [LICENSE](LICENSE)（MIT）授权。

## Noto Sans SC（HDMI / OLED 显示字库）

`sw/src/hdmi_font.c` 中的点阵由 `tools/gen_cjk_font.py` 从 Noto Sans SC 字体渲染生成，属于该字体的衍生物，
**按 SIL Open Font License 1.1 授权，不适用 MIT**。许可证全文见 [licenses/OFL-1.1.txt](licenses/OFL-1.1.txt)。

> © 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.

## 开发板 PS 配置参数

`vivado/ps7_preset.tcl` 中的 DDR、时钟、MIO 等参数，以及 `constr/sound_loc.xdc` 中的引脚对应关系，
依据正点原子启明星 ZYNQ7020 开发板资料盘中的例程与原理图整理。资料盘本身不包含在本仓库中，请从开发板厂商处获取。

## 工具链

构建需要 AMD Xilinx Vivado / SDK 2018.3，其生成的 IP、BSP、FSBL、比特流等都不包含在本仓库中。

## 商标

文中出现的产品和公司名称归各自所有者所有。本项目与正点原子、Sipeed、AMD Xilinx、Google、Adobe 均无关联。
