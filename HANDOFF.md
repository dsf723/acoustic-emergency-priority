> 这是开发过程中写给接手者的交接记录，按时间追加，保留了失败的方案和当时的判断，部分早期内容已被后面的章节取代。文中提到的 `rec/` 录音没有公开（见 `rec/README.md`）。

# 交接文档：启明星 ZYNQ7020 声源定位 + 声音识别

> 写于 2026-10-17。读者：接手继续开发的 AI / 工程师。
> 用户是电子信息工程本科生，项目用于比赛，**比赛要求识别救护车警笛**；用户用中文交流。
> 用户要的最终效果：OLED 显示声源**角度、远近等级（不显示米数）**，并识别**拍手**和**救护车警笛**（用户的警笛是"呜——"连续升降调）。

---

## 1. 一句话现状

- **定位（角度）**：已在真板上调通，用户确认"现在很准"。
- **OLED / 灯环 / 串口**：已调通。
- **拍手识别**：刚改为子帧级检测（见第 6 节），仿真全过，用 1 米真实录音回放基本正确，**还没在板上让用户验证新版本**。
- **警笛识别**：真板上已能检出用户的升降调警笛；本次已删除声级反推米数路径，统一改为相干度粗分远近。
- **远近等级**：按用户 2026-10-17 新决定，全部类别只输出近/中/远/未知；阈值仍待实测。
- **正在进行**：远近等级版软件已改动，测试与编译/下载结果见文末更新。旧分步采集仅完成第 1 步；下一轮优先持续警笛的远近分档验证。

---

## 2. 硬件与接线（已核实）

- 开发板：正点原子启明星 ZYNQ7020（XC7Z020CLG400-2），资料盘在 `F:\FPGA_project\【正点原子】启明星（V2）ZYNQ开发板资料盘（A盘）`
- 麦克风阵列：Sipeed 6+1（7×MSM261S4030H0，I2S；12×SK9822 灯环），半径 40mm（Sipeed D80 标准版）
- OLED：0.96 寸 4 针 I2C SSD1306，地址 0x3C
- 全部接底板扩展口 **J4**（Bank13，3.3V，仅 7020 有；依据底板原理图 V2.3.2 第 2 页）：

| J4 脚 | FPGA | 信号 | 接到 |
|---|---|---|---|
| 1 | V5 | mic_sck | 阵列 8 MIC_CK |
| 3 | U5 | mic_ws | 阵列 7 MIC_WS |
| 5 | V7 | mic_d[0] | 阵列 3 MIC_D0 |
| 7 | W8 | mic_d[1] | 阵列 4 MIC_D1 |
| 9 | U9 | mic_d[2] | 阵列 5 MIC_D2 |
| 11 | U10 | mic_d[3] | 阵列 6 MIC_D3 |
| 4 | T5 | led_ck | 阵列 9 |
| 6 | U7 | led_da | 阵列 10 |
| 8 | V8 | oled_iic_scl_io | OLED SCL |
| 10 | U8 | oled_iic_sda_io | OLED SDA |
| 37/39 | GND | | 阵列 2、OLED GND |
| 38 | +3.3V | | OLED VCC |
| 40 | +5V | | 阵列 1 VIN |

- 串口：底板 Type-C "USB_UART"（CH340C）→ PS UART0（MIO14/15），115200。**跳线帽 P5 必须在 UART1 一侧（1-3、2-4）**。电脑上是 **COM10**。
- JTAG：Digilent JTAG-HS1。BOOT_CFG 拨到 JTAG（两位 ON）。
- PS 配置（`vivado/ps7_preset.tcl`）取自资料盘 7020 例程：DDR3L MT41K256M16 RE-125×2、33.333MHz、UART0 MIO14/15、QSPI、SD0。

### 实测得到的阵列事实（重要，已写进配置）
- 通道：ch0~ch5 = 外圈 6 麦，**沿逆时针依次排列**；**ch6 恒为 0（空声道），中心麦是 ch7**。
- 方向约定（用户定义）：**0° = 指向 0 号麦，逆时针为正**（`DOA_MIC0_ANGLE_DEG 0`、`DOA_MIC_DIR +1`）。
- 灯环 0 号灯位置**未标定**（`LED0_ANGLE_DEG`、`LED_DIR`），用户说"大部分时间是对的"。

---

## 3. 工具链与常用命令（Windows，PowerShell 5.1）

- Vivado/SDK **2018.3**：`D:\Xilinx\Vivado\2018.3`、`D:\Xilinx\SDK\2018.3`
- Python：`D:\Python\python.exe`（有 numpy/scipy/matplotlib/pyserial/pdfplumber/pymupdf）
- PC 端 C 编译器：`D:\Xilinx\Vivado\2018.3\tps\mingw\6.2.0\win64.o\nt\bin\gcc.exe`

```powershell
# 硬件（只有改 RTL/约束/Block Design 才需要；必须在 vivado 目录下、直接用 PowerShell 调用）
Set-Location F:\FPGA_project\sound_localization\vivado
& D:\Xilinx\Vivado\2018.3\bin\vivado.bat -mode batch -source create_project.tcl
& D:\Xilinx\Vivado\2018.3\bin\vivado.bat -mode batch -source build.tcl
# 软件编译（删除并重建 sw/workspace，约 2 分钟）
Set-Location F:\FPGA_project\sound_localization\sw
& D:\Xilinx\SDK\2018.3\bin\xsct.bat create_sdk.tcl
# 下载运行（配置 FPGA + ps7_init + 下载 ELF + 运行）
& D:\Xilinx\SDK\2018.3\bin\xsct.bat F:\FPGA_project\sound_localization\sw\run_on_board.tcl
# PC 端算法测试（与板上相同的 doa.c / sound_class.c；必须 ALL PASSED）
python -I F:\FPGA_project\sound_localization\sw\test\gen_test.py
```

当前 `build/`（真实引脚 + 预设，WNS +12.6ns）和 `sw/workspace/`（最新固件）都是有效产物。

---

## 4. 目录与模块

| 路径 | 说明 |
|---|---|
| `rtl/mic_array_i2s_rx.v` | I2S 主机（SCK=3.125MHz，fs=48828.125Hz）+ 8 通道采集 + AXIS 打包（每帧 8 字 `{seq,ch,24bit}`，每包 1024 帧） |
| `sim/` | RTL 仿真（xsim，`run_sim.bat`，已通过） |
| `constr/sound_loc.xdc` | J4 引脚约束 |
| `vivado/*.tcl` | 建工程 / 构建 / PS 预设 |
| `sw/src/doa.c` | SRP-PHAT 定位 + 特征：频率掩码、最小值统计噪声、谱平坦度/质心/谱峰/音调度、相干度、短时峰值电平、峰均比、32 个子帧能量 |
| `sw/src/sound_class.c` | 拍手（子帧级）与警笛识别、距离/档位 |
| `sw/src/dir_filter.c` | 显示方向的加权投票滤波（约 0.75s 窗） |
| `sw/src/main.c` | DMA 双缓冲主循环、识别、OLED/灯环/串口、调试录音 |
| `sw/src/oled_ui.c`、`oled_ssd1306.c`、`font5x7.h` | OLED 界面与驱动（PS I2C0 经 EMIO，100kHz，按半页增量刷新，所有 I2C 等待有超时） |
| `sw/src/sk9822.c` | 灯环（警笛红 / 拍手绿 / 其他蓝） |
| `sw/src/doa_config.h` | **所有可调参数与标定值** |
| `sw/test/gen_test.py` + `test_doa.c` | PC 端回归测试（合成信号：定位精度、拍手 1/2/3 米、两种警笛、口哨/语音/噪声/拨弦音乐误报） |
| `tools/rec_dump.py` | 经 JTAG 控制/读出板上调试录音 |
| `tools/analyze_rec.py` | 把录音交给 PC 版算法重放，打印识别事件 |
| `tools/clap_proto.py` | 拍手子帧检测的 Python 原型（与 C 版逻辑相同） |
| `tools/calib_array.py` | 由拍手录音反推通道排列与 0° 方向 |
| `tools/serial_log.py` | 带时间戳记录串口（`python -I serial_log.py COM10 秒数 输出文件`） |
| `tools/doa_viewer.py` | PC 极坐标实时显示（读串口） |
| `rec/` | 实测数据（见第 9 节） |

串口输出格式见 `main.c` 文件头：`DOA,...`、`EVT,CLAP|SIREN,方位x10,档位`、`RNG,类别,档位`、`EVT,SIREN_END`、`LVL,...`、`FEA,...`。

---

## 5. 定位算法要点（已稳定，不建议大改）

- 每包 1024 帧（约 21ms）做一次：Hann + 1024 点 FFT → 频率掩码（只用高于该频点背景 10dB 的频点）→ 21 对 GCC-PHAT（300~6000Hz）→ 1/4 采样点时延网格 → 0~360° 每 1° SRP → 抛物线插值。
- 有效门限：能量高于背景 8dB 且 SRP 峰值比 ≥ 3.0（实测：1.5 时约 40% 是反射造成的反方向离群值）。
- 显示方向用 `dir_filter`（峰值比平方加权投票）。用真实日志回放：正确率 58% → 94%。
- 每块处理约 10.6ms（ARM -O2），加 OLED 半页约 6ms，预算 21ms。串口每行约 6ms（阻塞），不要每块都打印长行。

---

## 6. 声音识别（`sound_class.c`）

### 拍手（刚重写，需上板验证）
在 128 点窗、32 点步进的短时能量序列（预加重 0.9）上：起点比 3~20ms 前突增 ≥10dB → 20ms 内峰值高于背景 ≥12dB → 峰值后约 100ms 衰减 ≥10dB → ±50ms 峰均比 ≥8dB → 峰值块平坦度 ≥0.1、质心 ≥1000Hz → 150ms 不应期。检测延迟约 0.13s。
- 1 米真实拍手：峰值 −35.6~−39.9 dBFS，高于背景（约 −61）22~25dB，100ms 衰减 18~19dB，峰均比 11~13dB。
- 用户每次拍手力度差异可达 ±10dB，**按响度估距离对拍手误差可达 3 倍**，这是方法本身的限制，已告知用户。
- 房间里有大量键盘/脚步类短促声（比拍手弱 10~15dB），是拍手误报的主要来源；旧的块级版本在说话/音乐时误报严重，新版本还没有用说话/音乐的真实录音验证。

### 警笛
1 秒窗（48 块）内：音调块（谱峰 400~2000Hz、比中位数高 12dB、SNR≥8）占比 ≥60%，谱峰频率 10~90% 分位差 ≥100Hz，且音调块能量 10~90% 范围 ≤8dB（区分会衰减的乐音）才开始；占比 <30% 结束。警笛期间冻结背景噪声估计，方向门限放宽到峰值比 2.0。
- 真板上用户的升降调警笛：音调块 94~97%，谱峰约 600~1570Hz，能量范围中位 3~6dB，可以检出。
- 仿真里拨弦音乐曾 60% 误判为警笛，加了能量平稳条件后为 0；**真实音乐未验证**。

---

## 7. 远近等级（2026-10-17 用户新决定，替代原米数测距）

- 删除 `DIST_L1M_*`、`DIST_NEAR_M/FAR_M`、`distance_m` 及声级反推米数的计算。
- 所有类别统一用相干度粗分近/中/远；这只是与房间有关的线索，不是可靠测距。
- 参数 `RANGE_COH_NEAR=0.55`、`RANGE_COH_FAR=0.30` 为旧经验初值，**未完成实测标定**。
- 8 块窗口至少 5 块有效，SNR≥8dB，相干度极差≤0.15；边界留 ±0.04 未知区。
  当前块无效、窗口不稳定或证据不足时 UNKNOWN。拍手使用事件时保存的档位，常为未知。
- OLED 显示 RANGE / NEAR、MID、FAR、UNK，指针长度固定，不暗示米数。
- 串口事件改为 `EVT,类别,角度x10,档位`（删除厘米列）；每秒额外 `RNG,类别,档位`。
  `tools/doa_viewer.py` 已同步。PC 回放测试为兼容旧列索引保留恒为 -1 的内部占位列，非距离。
- 不再需要警笛“1 米电平”标定，应采集固定音量的近/中/远数据，评估是否能区分相干度。
  如果分布重叠明显，就使用 UNKNOWN，不能声称有可靠远近辨别能力。

---

## 8. 正在进行：分步骤标注采集（下一步从这里继续）

板上固件有"待触发连续录音"：
```powershell
python -I F:\FPGA_project\sound_localization\tools\rec_dump.py --arm 25   # 待命，第一个高于背景 25dB 的声音触发，录约 16s 自动停止（含触发前 1s）
python -I F:\FPGA_project\sound_localization\tools\rec_dump.py --status   # left=-1 未触发；frozen=1 已录完
python -I F:\FPGA_project\sound_localization\tools\rec_dump.py F:\FPGA_project\sound_localization\rec\L2_clap_2m.npz   # 读出（约 1 分钟）
python -I F:\FPGA_project\sound_localization\tools\analyze_rec.py F:\FPGA_project\sound_localization\rec\L2_clap_2m.npz --all
```
- 触发门限按的是**加窗频带能量**的 SNR，拍手在这个指标上只有 10~23dB；房间背景声常有 10~20dB，所以 15dB 门限一待命就被误触发（刚才第 2 步就是这样：已触发，需重新 `--arm`）。建议 2/3 米拍手用 18~20 并在用户回复"准备好"之后立刻待命，或改为用 `peak_level_db` 触发。
- 流程：每步一条消息。用户做完回复后读出、分析，再给下一步。

| 步骤 | 内容 | 状态 |
|---|---|---|
| 1 | 1 米拍手 5 下 | 完成 → `rec/L1_clap_1m.npz`（新算法检出 4 个，方向 7~14°，1 米标定由此得出） |
| 2 | 2 米拍手 5 下 | **待做（需重新待命）** |
| 3 | 3 米拍手 5 下 | 待做 |
| 4~6 | 1/2/3 米放警笛各 10 秒 | 待做（改用于近/中/远相干度分布验证，不拟合米数） |
| 7 | 正常说话 15 秒 | 待做（验证拍手/警笛误报） |
| 8 | 放一首歌 15 秒 | 待做（验证警笛误报） |

采集完成后：用 `analyze_rec.py` 回放调参（改 `doa_config.h` → `gen_test.py` 必须仍 ALL PASSED → `create_sdk.tcl` → `run_on_board.tcl`），再让用户在板上实际验证 OLED 显示。

---

## 9. 数据文件（`rec/`）

| 文件 | 内容 |
|---|---|
| `claps1.npz` | 早期"只录响亮块"录音；拍手离阵列很近（峰值接近满幅），**不能用于 1 米标定**；块 111/122/139/150/156 是 5 次拍手，用于标定了通道排列 |
| `serial_log2.txt` | 标定方向后的串口日志，用于确定峰值比门限 3.0 和投票滤波 |
| `serial_log3.txt` | 定位稳定后的日志（含旧近场测距输出） |
| `serial_log4.txt` | 第一次识别测试（含 FEA 特征），步骤未分段、混有大量其他声音；警笛段约 603~612s、703~720s、722~732s |
| `L1_clap_1m.npz` | 标注：1 米拍手（连续 16s） |

---

## 10. 调试手段与坑

- **板上变量可经 JTAG 读写**（不用重新编译）：`debug_fea`（写 1 输出 FEA 特征行，只在有声块输出）、`rec_mode/rec_idx/rec_freeze/rec_left/rec_trig_db`（录音）。地址用 `arm-none-eabi-nm sw/workspace/sound_loc/Debug/sound_loc.elf` 查。读 PL 寄存器（0x4xxxxxxx）要用 `mrd -force`。
- 停 CPU 看卡在哪：xsct `stop; bt; con`，地址用 `arm-none-eabi-addr2line -f -e sound_loc.elf` 解析。
- **用 Bash 工具的 heredoc 里写含 `\r\n`、`\0`、`\\` 的替换时会被转义吃掉**（曾经把 `\r\n` 写成真换行、把 `'\0'` 写成 NUL 字节）。含反斜杠的 C 代码修改请用 Edit/Write 工具。
- Vivado 用 `cmd /c xxx.bat` 调用时多次在 `create_bd_design` 报 "couldn't read file mig_utils.tcl/testbench.tcl"；**在 vivado 目录下用 PowerShell `&` 直接调用**则正常。
- `puts` 里写中文会乱码，Tcl 输出信息用英文。
- 串口一次只能被一个程序打开（`serial_log.py` 后台运行时用户不能开 XCOM）。`serial_log.py` 曾出现过不写文件的卡住现象，长时间记录时注意检查文件更新时间。
- 用户的 PowerShell 是 5.1：不支持 `&&`、`cd /d`，给用户的命令用 `& 完整路径` 的形式。
- 用户回复通常很短；需要用户动手的步骤要写清楚距离、次数、方向，一次只给一步。

---

## 11. 已知问题 / 待办（按优先级）

1. 新版上板验证无米数；优先采固定音量警笛的近/中/远录音，检验相干度分档是否有效。
2. 用说话/音乐真实录音检查新拍手检测与警笛的误报，必要时调 `SC_CLAP_*`、`SC_SIREN_*`。
3. 按真实数据标定所有类别共用的 `RANGE_COH_NEAR/FAR`，保留未知区。
4. 灯环方向标定（`LED0_ANGLE_DEG`、`LED_DIR`）。
5. README 已更新远近说明和串口格式；比赛指标以真实标注测试为准。
6. 脱离电脑运行：`sw/make_boot.tcl`（2026-10-24 已做，见第 20 节）。

## 12. 2026-10-17 远近等级版更新与验证

- 用户明确取消米数测距，保留角度、声音识别和近/中/远/未知。代码和 README 已同步。
- PC 回归 `gen_test.py`：ALL PASSED；新增 `test_range.c` 检查预热、近中远、低 SNR、
  无效方向、边界、波动、NaN/越界及初始化重置；不代表真实环境远近识别准确率。
- `L1_clap_1m.npz` 回放仍有 4 个拍手事件、0 个警笛块，远近均为未知。
- OLED 五种状态预览已生成到 `docs/oled_preview.png`，包括有声 UNK 和无声 ---。
- SDK 应用重新编译成功，最终应用编译未出现 warning/error；ELF SHA256：
  `890f45957dcdf0f7f6cc6cf7468d062fe331f32a11ae115afcda77cdd219e7d3`。
- 已通过 `run_on_board.tcl` 成功下载 BIT 和 ELF 并运行；COM10 记录见
  `rec/range_only_startup_20261017.txt`，可见 OLED ready、帧计数递增和新 RNG 输出。
- 本次串口启动后记录约 11 秒，未要求用户播放标注警笛；不能据此声称远近档位已实测有效。
  启动出现一次短包（32752/32768），程序丢弃后继续；overrun 粘滞位为 1，旧程序下载前
  也为 1。尚未区分启动期积累还是稳态丢帧，连续录音验收前应单独检查。
- 原源码、文档与旧 ELF 备份：`build/before_range_only_20261017.zip`。
- 新增 `sw/rebuild_app.tcl`：同步 sw/src 后仅清理重建应用，保留硬件平台与 BSP。
- **下一步先让用户确认 OLED 无米数；再做固定音量、标注位置的持续警笛录音，验证/标定
  RANGE_*，同时检查采集连续性。当前阈值暂定，不能承诺某档对应固定多少米。**

## 13. 2026-10-17 警笛方向偶发跳变处理

- 用户反馈：声源约 0°，大部分正确，偶尔显示 220°；类别 SIREN，远近 UNK。
- 新采 `rec/before_direction_guard_20261017.txt` 的 12 秒串口没有复现 220°，主要为约 0°
  的普通声音方向，不能断言已确定声学根因。代码复核发现原投票滤波没有大跳变确认。
- `dir_filter` 新增 guard 模式，`main.c` 仅在 sc.siren_active 时开启；单次权重上限 25。
  方向变化>60°时，候选簇在最近18块内需至少5个不同块、首尾跨度至少8块、最近支持距当前
  ≤3块，且总权重≥原方向簇1.25倍，才允许切换。不确认时保留旧方向，不续期其时间戳。
  窗口失效后允许重新捕获；真实大角度移动存在延迟。非警笛保持原投票响应（仍有权重上限）。
- 新增 `sw/test/test_direction_guard.c` 并接入 `gen_test.py`：孤立高权重、同块重复观测、
  359°/1°环绕、持续真实换方向、静音过期、guard关闭等行为通过；完整回归 ALL PASSED。
- SDK 重编译成功且已通过 JTAG 下载运行；启动日志 `rec/direction_guard_startup_20261017.txt`。
  旧远近等级版 ELF 留在 `build/sound_loc_before_direction_guard.elf` 便于回退。
- **待用户在原0°位置重播警笛验证，再移到90°检查响应；尚未实测证明220°跳变已消除。**

## 14. 方向确认版后的新反馈与诊断准备

- 用户随后反馈：警笛有时漏检，角度还会瞬间跳到139°，说明第13节修改未充分解决实测问题。
- 已请求用户在0°、约1米、固定音量持续播放约40秒并回复“正在播放”，用于标注连续采集。
- 已保存旧响亮块缓存 `rec/siren_fault_unlabelled_20261017.npz`：591块、174处块号间断，
  **非连续、未标注**，不能当作本次警笛漏检的有效复现。早期拼接分析文本不应作为识别率依据。
- `tools/rec_dump.py` 新增 `--continuous`：立即连续录满768块约16.1秒，不依赖SNR触发，
  可覆盖漏检时的弱声块；使用前先保存必要旧数据。录完 `--status` 应 frozen=1。
- `tools/analyze_rec.py` 默认拒绝含块号间断的录音，避免把响亮块拼接后产生伪造的时序事件；
  `--allow-gaps` 仅供特征排查，不能用于时序分类验收。
- 等用户回复正在播放后：`--continuous`，同时串口记录约20秒；确认录完后导出新npz，检查
  块连续性、警笛音调/SNR/频率起伏/能量稳定性条件、异常角度的置信度及滤波是否在掉识别时关闭。
  **此阶段未再次修改/下载算法，避免在没有故障数据时继续盲目调阈值。**

## 15. 2026-10-19 开始救护车目标识别流程

- 用户已授权开始。提供参考文件：`F:/FPGA_project/制片帮素材_188809_救护车的警笛声免费音效.mp3`。
  实际播放方式为**手机+外接音箱**，不要自动从电脑播。当前UART是 **COM11**（由旧COM10变化）。
- 新部署 `SC_TARGET_ONLY=1`：只输出 NONE/SIREN，停止拍手和普通声显示。仍是规则警笛候选，
  **没有训练救护车专用模型，也没有验证能排除警车/消防车**。
- 新增 `target_output.c/h`，仅将确认警笛的合格方向送入历史；新事件清历史、至少3个不同块
  确认，0.25秒无新鲜支持则方向UNK、灯灭。OLED has_sound与direction_valid分开。
- 数据错误/短包/DMA故障重置识别和方向、解冻背景估计，并在故障时清OLED，避免旧目标悬挂。
- 新增累计 `capture_seq_errors/capture_short_packets/capture_dma_errors`，串口CAP输出。
  启动一次短包为已知现象，采集验收比较错误计数增量，不把粘滞overrun=1当稳态错误数。
- 串口DOA/特征节流，OLED在长遥测行所在块暂停半页刷新，降低阻塞串口对DMA预算的影响。
- 旧算法回归用SC_TARGET_ONLY=0；新增目标模式API及整段合成音频回归，ALL PASSED。
  `tools/test_capture_labelled.py` 10项离线测试通过；OLED五种状态预览已更新。
- 新增带标签采集工具 `tools/capture_labelled.py`，详见README及docs/ambulance_dataset_plan.md。
  metadata标签为人工提供未核验；WAV整数头48828Hz，真实fs=48828.125Hz，未重采样。
- 第一段实录：`rec/dataset/ambulance/20261019T081337_967373Z_zhipianbang_188809_1950cc78/`，
  raw.npz共768块16.106秒，0块号间断，CAP delta=0；但实录没有触发警笛，信号内容待重录复核。
  对比报告 `build/reference_capture_comparison.json` 未找到可信参考片段对齐，不能断言手机没播。
  实录400~2400Hz带内电平约-70~-77dBFS，参考明显615/791Hz等成分不清楚。
  不应直接放宽规则或将这条结构complete的样本视为可用训练正样本。
- 参考MP3本身48.74秒、22050Hz立体声；在合成理想传播中规则基本匹配，存在中段空白。
  source SHA256 `f32e34132a35a9b7c187d50bb4a591a6bc4dcc57585590619b1afc79a399476f`。
  分析工具为tools/analyze_reference_clip.py、tools/compare_reference_capture.py，报告在build/。
  MP3解码依赖项目本地build/python_deps/miniaudio==1.71，未改全局Python环境。
- 最后SDK ELF SHA256 `d885b04d9ec2fc4527de5784d95564ea66c8363db80b846075b74cdfe526ad7f`，
  已编译并最终JTAG下载成功（下载输出Successfully downloaded，程序运行）。旧源码/ELF备份
  `build/before_target_only_20261019.zip`。
- 正在等用户回复**“可以重录”**：手机单曲循环、外接音箱0°约1m、阵列处清楚可闻，先暂停。
  用户就绪后先采安静背景，再提示播放并重录目标，分析时用背景预段初始化噪声估计。
- 已知规则边界：警笛触发后无缝切换稳频音，结束条件只看音调占比，可能继续SIREN。
  后续需增加有限模式失效宽限/测试；本轮没有通过盲目改门限来处理。

## 16. 2026-10-19 用户“继续完成”后的断连与离线推进

- 尝试背景录音时设备再次断开：JTAG目标为空，COM11消失，电脑仅余蓝牙虚拟串口。
  失败会话为 `rec/dataset/background/20261019T135855_855628Z_room_background_20261019_456d7a8a/`，
  status=failed，没有原始录音，不能进训练集。已请求用户重新上电、接JTAG与USB_UART，回复“已重新连接”。
- 不要把上一条“继续完成”当作背景已录成功。恢复连接后须重新检测串口号，匹配ELF/运行程序，
  然后先采背景，再提示手机+外接音箱循环播放目标重录。
- 新增中文本地菜单 `开始标注采集.cmd` → `tools/collect_session.py`，让操作者在电脑按回车
  准确开始，避免聊天收发延迟导致录到MP3中间空白或片尾。
- 新增 `tools/review_recording.py`，仅对status=complete且结构校验passed样本记录人工听音标签复核。
  第一条救护车实录仍为content_review_status=needs_rerecording，未被自动批准。
- tools/test_capture_labelled.py在菜单提示修改后10项测试仍通过。
- 离线PC专用分类器训练/评估工具正在补充；它会拒绝未复核样本、隔离同source-id来源，
  没有足够独立正负来源时不生成模型。当前尚无实测训练模型，不能声称只识别救护车已经实现。

## 17. 2026-10-19 晚间恢复连接后的采集与训练入口

- 用户确认已重新连接，COM11和JTAG恢复；重新下载D885...版本成功，程序正在运行。
- 背景有效结构会话：`rec/dataset/background/20261019T140447_316549Z_room_background_20261019_fc230556/`。
  768块、0块号间断、CAP delta=0，离线规则无SIREN；标签仍待内容复核。
- 用户确认手机+外接音箱正在循环播放同一参考音频，第二条目标会话：
  `rec/dataset/ambulance/20261019T141016_786958Z_zhipianbang_188809_78651b9c/`。
  正在采集/导出，必须检查最终metadata和日志，不要预先宣称识别成功。
- 工具analyze_rec.py新增 `--background 背景raw.npz`：最多2秒背景预段初始化C算法噪声估计；
  明确属于离线初始化拼接，不是跨会话连续实录。旧第一条弱目标加背景后仍0/768警笛块。
- PC训练工具tools/train_ambulance.py与8项测试已完成并通过：8kHz、2秒log-mel/节奏特征、
  StandardScaler+LogisticRegression；source-id分组隔离train/validation/test，阈值只用val。
  当前真实dataset --dry-run拒绝训练（没有approved样本），未生成实测模型。
  最少每个二分类3个独立来源，仅为运行下限，不代表比赛泛化充分；还需相似警笛负例。

## 18. 2026-10-23 显示改为 HDMI（已上板，用户确认画面正常）

- 用户要求“屏幕换成 HDMI 屏幕”。OLED 代码保留（未接 OLED 时自动跳过），HDMI 为主显示。
- **PL**：新增 `rtl/hdmi_fb.v`（模块引用，BD 中实例名 `hdmi`）。640x480@60Hz（HDMI 必须支持的格式），
  DVI 编码（无数据岛）；帧缓冲 38400×32bit 双口 BRAM，每像素 4bit、16 色固定调色板，
  每字 8 像素低位在左，字地址 y*80+x/8。AXI4-Lite 写（只支持整字），基址 **0x43C00000**（256KB），
  状态寄存器 0x3FFF0：bit0=HPD、bit31:8=帧计数。像素时钟由 PL 50MHz 晶振 U18 经 MMCM 生成
  （M=20.125，像素 25.156MHz、串行 125.78MHz），OSERDESE2 10:1 + OBUFDS TMDS_33。
- **引脚**（资料盘 PHOSPHOR_ZYNQ_IO.xdc / 底板原理图第 5 页）：tmds_data_p[0..2]=G19/K19/J20，
  tmds_clk_p=J18（N 端自动 G20/J19/H20/H18），hdmi_hpd=L19，sys_clk=U18。
- **资源/时序**：BRAM 65/140（帧缓冲约 64），LUT 5.6%，MMCM 1，WNS +13.6ns。
  XDC 设为 PROCESSING_ORDER LATE，使 sys_clk 与 clk_fpga_0 的异步时钟分组生效（之前因找不到
  clk_fpga_0 报 CRITICAL WARNING，未生效但时序本身满足）。
- **仿真**：`sim/run_sim_hdmi.bat`（需 -L unisims_ver -L secureip）——AXI 写读回、状态寄存器、
  第二帧像素颜色、309764 个 TMDS 符号解码、直流平衡（最大 8）、串行端控制符号对齐 100%，TEST PASSED。
- **PS**：`sw/src/hdmi_ui.c/h`（纯 C 绘图与界面，PC 预览 `sw/test/preview_hdmi.py` → `docs/hdmi_preview.png`），
  `sw/src/hdmi_out.c/h`（DDR 影子帧 → 只写变化的字，每块最多 6000 字约 1ms）。
  `main.c`：每 6 块重画一次（约 8 帧/秒），故障时随 OLED 一并清空显示状态。
  界面：标题栏；左侧 TARGET（SIREN 红 / ----）、ANGLE（度数 / UNK / ---）、RANGE（NEAR 绿 / MID 黄 /
  FAR 橙 / UNK）；右侧罗盘（0° 朝上、逆时针为正，与 OLED、阵列约定一致）；检测到目标时红框与
  “ALERT: SIREN DETECTED”；右下角模式字样（现为 "SIREN ONLY - FOCUS DOA"，见第 19 节）。
- **上板记录**：第一次下载后几秒 PL 丢失配置（`fpga -state` = not configured，DAP AP 错误，CPU 卡在 AXI 访问），
  断电重启后旧版（无 HDMI）与 HDMI 版各稳定运行 35 秒以上，视频帧计数约 60/秒。推测为插接显示器时
  的供电跌落，**未最终确认**；建议用 DC 适配器供电、断电插拔 HDMI。HPD 读数一直为 0（显示器仍正常显示，
  输出不依赖 HPD）。
- 备份：HDMI 改动前 `build/` 中的非工程文件与旧 ELF 在 `backup_build_extras_20261023/`；
  旧无 HDMI 比特流可从其中 `system_wrapper.hdf`（zip）解出。
- 待办：用真实警笛确认 HDMI 画面随识别结果刷新（角度/红框/远近）。

## 19. 2026-10-23 抗干扰：方向只跟警笛走（`siren_focus.c`）

- 用户反馈：能识别救护车声，但有其他声音干扰时，显示方位和远近会跟着干扰源变。
  原因：宽带 SRP-PHAT 用本块所有高于背景的频点，干扰比警笛响时方向、相干度都被拉走。
- 做法（链路：`doa_process` → **`siren_focus_apply`** → `sc_process` → `target_output_update`，
  `main.c` 与 PC 测试 `sw/test/test_target_doa.c` 相同）：
  1. `doa.c` 新增 `doa_find_peaks()`（400~2500Hz 突出谱峰）与 `doa_focus_at()`（只用某个音调分量
     基频各次谐波处的频点做 GCC-PHAT+SRP；每次谐波本身必须是突出谱峰，否则那些频点只有别的声源）。
     相位按 lag 递推，每频点只算一次 cos/sin（PC 上全链路只多约 12% 时间）。
  2. `siren_focus.c`："方向槽"——每块最多聚焦定位 4 个分量（谐波去重，警笛连续性加分排前），
     按方位（±25°）归入最多 4 个槽；每槽统计最近 1 秒：音调块占比、频率起伏、电平起伏、
     "无法解释的频率跳变"（既不连续，也不是回到用过的频率/整数倍）。
     完全符合警笛判据（占比≥0.6、起伏≥100Hz、电平范围≤8dB、跳变≤1）的槽为严格目标，之后保持（滞回）；
     宽带识别已确认警笛但没有严格目标时（真实房间混响使窄带方向散开），用备用目标（占比≥0.4、跳变≤2）。
  3. **识别仍由 `sound_class` 按宽带特征判断**（不改原有规则与回归）；方向、峰值比、相干度（远近档位）
     只取目标槽的观测；本块目标槽没有观测时不给方向，绝不退回宽带结果。
  4. 参数在 `doa_config.h` 的 `DOA_FOCUS_*`、`SF_*`；`-DDOA_FOCUS_ENABLE=0` 即恢复原行为。
- 失败过的方案（不要再走）：单一频率跟踪 + "最先锁定的方向"——合成测试通过但实录锁到少数方向；
  方向多数表决（2 秒窗）——3 秒的音乐干扰成为多数；只按方向槽判定识别——实录混响使每个方向的音调占比
  都不到 0.6，识别率从 83% 掉到 0%。
- 回归：`sw/test/interference_test.py`（已并入 `gen_test.py`，**ALL PASSED**）：
  升降调/两音警笛 120°，更响的说话 300°（+12dB）、音乐 210°、敲击 30°；以及**音乐比警笛先响**的情形。
  聚焦关：指向错误（>30°）13~41%，音乐段误差约 87°；聚焦开：指向错误 0%，音乐段 2~5°，警笛检出 100%。
  无警笛（音乐/说话/敲击/1200Hz 口哨）误报 0%；单独警笛 6 个方位 × 2 种，误差 ≤0.1°。
- 实录（`rec/dataset/ambulance/20261019T141016…`，标注 0° 1m，未核实）：检出 83%（与原来相同），
  显示方向稳定在 140~155°（原宽带版本中间还会跳到 330° 等）。**标注 0° 与测得约 150° 不一致仍未解决**：
  可能是喇叭实际不在 0°，也可能是房间反射使窄带声音的视在方向偏移。需要用户做已知方向的实测。
  另一段 `20261019T081337…` 原版与新版都是 0% 检出（与本次改动无关）。
- 上板：2026-10-23 23:39 重新编译 ELF（硬件不变），已下载运行。实测 `LVL` 处理耗时（doa_process+siren_focus）
  10.7~14.9ms / 每块 21ms 预算；`CAP,0,1,0` 与 overrun=1 均为启动时的一次，不再增长；HDMI 帧计数 62/秒
  （开机打印 "video frames 47 -> 47" 只是延时短于一帧，不是故障）。尚未用真实警笛加干扰在板上验证。
- 待办：用户在已知方向放警笛（如 90°），先无干扰 15 秒，再加手机放音乐/说话从另一方向干扰，
  看 HDMI 方向是否保持；`tools/rec_dump.py --continuous` 录下来离线复核。

## 20. 2026-10-24 上电自启动（QSPI）

- 用户反馈"上电不运行"：JTAG 下载只在 RAM 中，断电即失（实测上电后 `fpga -state` = not configured）。
- 新增 `sw/make_boot.tcl`：创建并编译 `workspace/fsbl`（Zynq FSBL）→ bootgen 生成 `sw/boot/BOOT.bin`
  （FSBL + system_wrapper.bit + sound_loc.elf，约 4.2MB）→ program_flash 烧 QSPI（qspi_single，W25Q256）。
  参数 `noflash` 只生成 BOOT.bin（可拷 SD 卡根目录，拨码两位 OFF 用 SD 启动）。
- 2026-10-24 已烧写成功（22 秒）。拨码：JTAG = 两位 ON；**QSPI = 开关1 ON、开关2 OFF**（规格书 3.3 节）。
- 以后每次改代码：`rebuild_app.tcl` → `make_boot.tcl`（烧写时拨码要在 JTAG 位置）。
- 注意：自启动时 JTAG 调试录音（rec_dump.py）仍可用，xsct 连上即可读内存。

## 21. 2026-10-24 "只能判断大致方位、角度一直在变"

- **硬件**：本次串口 `LVL` 中 ch2、ch3 恒为 -200dBFS（全零），即 `mic_d[1]`（阵列 4 脚 MIC_D1 → J4 第 7 脚 → W8）
  没有数据，只剩 4 个外圈麦工作；2026-10-23 的日志中各通道都正常，判断为杜邦线松脱。先让用户查线。
- **显示稳定**（`target_output.c` + `doa_config.h` 的 `TARGET_DISPLAY_*`）：显示值为方向长时圆周平均（α=0.08/有效块），
  偏离当前显示 >12° 才更新，取整到 5°；重新确认或方向滤波确认大跳变（>60°）时从新方向重新开始。
  `TARGET_DIR_FRESH_BLOCKS` 由 12 改为 48（约 1s 无方向证据才显示 UNK）。
  实录（20261019T141016）：相邻画面角度变化的比例 62% → 4%，显示角度 90% 偏离 ±17.5° → ±9.4°，显示块 368 → 561。
  新增移动警笛回归：60°→200° 换位 0.9s 跟上；15°/s 匀速转动时显示滞后约 12.5°（死区所致）。
- 已重新编译、烧写 QSPI 并经 JTAG 运行。

## 22. 2026-10-24 角度 1° 分辨率 + 中文 HDMI 界面

- 用户：线已插好（串口 ch0~5、ch7 均约 -35dBFS 正常）；嫌只按 5° 变化，要求界面中文、更高级。
- 显示：`TARGET_DISPLAY_STEP_DEG` 5→1，`TARGET_DISPLAY_DEADBAND_DEG` 12→3（实录相邻画面变化 18%，90% 变化 ≤3°；
  匀速转动 15°/s 滞后约 7°）。注意分辨率 1° 不等于精度 1°：房间里警笛实际误差约 ±5~10°。
- 界面：`sw/src/hdmi_ui.c` 重写（硬件与调色板未改）。标题栏 + 状态标签（监听中/警报中）、左侧三卡片
  （识别目标 / 声源方位大号整数度 / 距离估计 + 信号格）、右侧罗盘（10° 细刻度、30° 主刻度、中心阵列示意、
  0 号麦青色、方向波束+指针；警报红圈，方向未知黄圈）、底部状态栏、警报红框。静态背景缓存，每次只画动态部分；
  `main.c` 只在显示内容变化时重画。板上实测：首次 2.2ms，之后每次 0.8ms。
- 字库：`tools/gen_cjk_font.py` 用 Noto Sans SC（OFL）生成 `sw/src/hdmi_font.c/h`（2bit 抗锯齿，16/22/34/80px，约 50KB），
  汉字从 hdmi_ui.c、main.c 字符串自动收集。**改界面文字后必须重新运行它**；`sw/test/preview_hdmi.py`
  缺字时报错，并输出 `docs/hdmi_preview.png`。
- 已重新编译、烧写 QSPI、JTAG 运行。

## 23. 2026-10-24 "有噪声时不能准确识别角度"

合成实验（警笛 120°，150~4000Hz 宽带噪声，噪声总功率比警笛高 0/+6/+10dB；方向噪声在 300° 或各麦独立的散射噪声）发现：
1. **识别失败**：持续噪声下整体信噪比（energy_db - noise_db）只有 0.6~2dB，过不了 `SC_SIREN_MIN_SNR_DB`。
   → `doa_result_t.peak_snr_db`（最强谱峰高于同频点背景）；`sc_process` 用 max(整体, 谱峰) 信噪比；
   `SC_SIREN_TONAL_DB` 12 → 9（噪声 +6dB 时谱峰只突出约 11dB）。合成负样本、实录背景/拍手均 0 误报；
   实录 20261019T141016 检出 83% → 96%。
2. **方向被拉到噪声源**：`psd_noise`（下包络）每秒只升 0.5dB，噪声刚出现时谐波两侧的纯噪声频点都过了掩码。
   → `doa.c noise_xspec_update()`：频点不高于两侧邻近频点平均 6dB 时视为纯噪声，按 α=0.05 更新 `nx_psd`；
   聚焦定位只用高于 `nx_psd` 6dB（`DOA_NX_SEL_DB`）的频点（`DOA_NX_SELECT`）；`DOA_FOCUS_MIN_BINS` 4 → 2。
   互功率谱减法（`DOA_NX_SUBTRACT`）试过：对平稳噪声有效，但音乐被当成"噪声"平均后减去会把警笛方向拉偏，默认关闭。
3. **方向槽统计被串扰**：两声源频点重叠时，对方的分量偶尔被定位到本方向，使跳变数、电平范围变坏。
   → 槽内观测分"成员/离群点"：不连续的观测先当离群点（不进统计、不改轨迹），连续 `SF_REANCHOR_N`=2 个彼此连续才算真跳变。
   谐波关系也按预测频率判断（升降调扫频时基频/谐波切换）。
4. **备用目标选成了音乐**：→ `fallback_score` = 占比 × 响度平稳度 − 0.1×跳变，换目标须高出 0.15；
   警笛仍确认时严格目标只在占比 < `SF_HOLD_FRAC`(0.1) 才放弃；`SF_FALLBACK_FRAC` 0.4 → 0.3。
- 结果（已并入 interference_test / gen_test，ALL PASSED）：方向噪声/散射噪声 0dB、+6dB 全部 0% 指错；
  +6dB 两音警笛只在 17~37% 的块显示方向（其余显示"未知"，不指错）；+10dB 以上识别不到（谱峰只突出约 8dB）。
  实录显示稳定在 126~163°。
- 注意：合成噪声没有房间混响；真实房间请用户用 `tools/rec_dump.py --continuous` 录一段"警笛+噪声"回放复核。

## 24. 2026-10-24 安静时误报救护车（第 23 节改动引入）

- 现象：板上安静房间 4 分钟内约 5 次误报（237 秒里 57 秒处于"警笛"状态）。房间有持续的嗡嗡声（330~420Hz 及 640~700Hz
  一带的起伏音调），tonality 中位数约 16dB。原来靠整体信噪比 >= 8dB 挡住；第 23 节加的"谱峰信噪比"以下包络 `psd_noise`
  为基准，起伏的嗡嗡声最低点远低于平均，谱峰信噪比中位数达 14dB，于是通过。
- 修改：
  1. `peak_snr_db` / `doa_focus_t.snr_db` 改为相对 `psd_bg_db`（各频点 dB 域长时平均，`DOA_BG_ALPHA` = 1/240 约 5s，
     警笛期间随 noise_freeze 冻结）。持续嗡嗡声几秒后成为背景；刚响起的警笛 1 秒只让背景升几 dB。
     安静房间谱峰信噪比中位数 14 → 4dB。
  2. `SC_SIREN_ON_FRAC` 0.6 → 0.7（误报段 1 秒内音调块最多 0.52；噪声 +6dB 的警笛 0.81~0.98、实录警笛 1.0）。
     方向槽的严格目标占比单独用 `SF_STRICT_FRAC` = 0.6（只选方向，不影响识别）。
- 实录回归：`rec/room_now_20261024.npz`（安静房间 16s）、`rec/false_alarm_quiet_20261024.npz`（旧固件误报时录的 16s）
  已加入 interference_test（要求误报 <= 1%），均为 0%。实录警笛检出 76%（显示方向不变，126~157°）。
- 第二轮（同日）：新固件 4 分钟仍有 1 次 2 秒误报。录下 `rec/false_alarm_quiet2_20261024.npz` 分析：是**人声**
  （基频 180~226Hz 的谐波列，哼唱/说话，比背景高 9~18dB，音调度 20~32dB，响度平稳），偶然凑够了开始条件。
  试过：谐波梳状检测（1024 点分辨率不够，且房间嗡嗡声也成梳状，误标 10% 警笛块）、频率变化规律（人声滑音也"连续"）——都分不开。
  有效区分：开始条件的**持续时间**——人声最长连续 16 块，警笛（含噪声、干扰）>= 113 块。
  → `SC_SIREN_CONFIRM_BLOCKS` = 36（约 0.75s）连续满足才确认，识别延迟约 1.75s。
  `test_target_mode.c` 的确认时刻相应调整。实录警笛检出 63%（多出的延迟 + 录音中间的停顿后需重新确认），显示方向不变。

## 25. 2026-10-24 交通杂声误判为救护车

- 现场没抓到（循环录音 16 次，用户当时没在放交通声）。用合成交通噪声复现：发动机谐波（基频 40~160Hz 滑动）、
  短喇叭单独都不误报；**2.5 秒长喇叭（420/510Hz 两音同时响）+ 发动机**误报。原因：两音同时存在时最强谱峰在两音间来回跳，
  满足了"频率起伏 >= 100Hz"，响度又平稳、持续够长。
- 区分：警笛两音是**轮流**响的（一个音停留零点几秒），喇叭两音、人声/发动机谐波是**同时**存在的。
  `sound_class.c` 新增"谱峰跳变率"：相邻音调块（间隔 <= 2 块）谱峰差 > `SC_SIREN_FLIP_HZ`(60Hz) 且非整数倍记一次跳变，
  1 秒窗口内跳变比例 <= `SC_SIREN_MAX_FLIP`(0.2) 才满足开始条件（只管开始，确认后的保持不变）。
  实测：合成长喇叭 0.17~0.48、实录人声 0.16~0.36；各种合成警笛中位数 <= 0.06，实录警笛 90% <= 0.15。
- `interference_test.py` 新增 `scenario_traffic()`（30s，要求误报 <= 1%）：0%。ALL PASSED；实录警笛检出 63%（不变）。
- 已编译、烧写 QSPI、JTAG 运行。**仍需用户真实交通声录音复核**（真实喇叭、刹车尖叫、电动车提示音可能不同）。

## 26. 2026-10-24 距离识别失败 → 改为按警笛音量估距离（需 1 米标定）

- 诊断：实录（约 1 米）警笛期间 97% 显示 UNKNOWN，其余近/中/远乱跳。相干度（直达/混响线索）对持续单音无效：
  同一位置每块 0.28~0.68，8 块窗口波动 > 0.15 的占 60%。
- 新方案（`SC_TARGET_ONLY` 下；拍手/旧模式仍用相干度，`test_range.c` 不变）：
  `doa_focus_t.power_db` = 聚焦所用各谐波频点功率和 → `doa_result_t.target_level_db`（siren_focus 填，其余 -200）；
  `sound_class.c range_level_update()`：最近 48 个方向有效块音量的中位数，d = 10^((参考 - 音量)/20) 米，
  < 1.5m 近、> 4m 远，15% 滞回；每次警笛重新累计。`snd_state_t.distance_m`。
  参考值默认 `RANGE_REF_LEVEL_DB` = -3.1dB（2026-10-19 实录约 1 米、喇叭音量未知），**现场须标定**：
  喇叭放 1 米、警笛响着时串口发 `C`（`sc_range_calibrate(1.0)`），回 `CAL,OK,<x10>` 或 `CAL,FAIL,NO_SIREN`；
  每秒输出 `DST,<米x100或-1>,<参考x10>`。HDMI 距离卡片右上角显示"约 x.x 米"。标定值断电丢失。
- 结果：实录（位置不动）估计 0.66~1.21m、近距离 97%（原 97% 未知）；合成 1m 标定后 2m/4m/0.6m 估计准确
  （无混响，真实房间超过混响半径后远处会偏小）。interference_test 加入合成距离阶梯与实录近距离检查，ALL PASSED。
- 已编译、烧写 QSPI 成功；随后板子断电，**串口 'C' 命令未能在板上验证**（发送时板子可能正在断电）。
- "有时识别不到"：本轮没有现场数据，未改识别规则。相关改动累计使确认延迟约 1.75s；需用户按距离录标注数据复核。

## 27. 2026-10-24 路口信号灯控制（作品："应急车辆声学让行路侧协同终端"），OLED 代替数码管

- 用户作品构想（文字见本节下方附注）：听警笛来向 → 控制信号灯优先放行（绿灯延长、红灯早断）、LED 灯组展示。
  当前在启明星 7020 上实现（用户文案中的 MLK-AFH03-AFC03 / RK3576 / CAN 尚未涉及）。
- **硬件**：BD 新增 `axi_gpio_tl`（15 位全输出，默认 0）→ 端口 `tl_out[14:0]`；引脚表在 `constr/sound_loc.xdc` 末尾与 README。
  J4.12~26（Bank13）：bit0~2 北口红黄绿 T9/V6/Y6，3~5 南口 W6/Y7/Y9，6~8 东口 Y8/W9/V10，9~11 西口 W10/V11/Y13，
  12 优先指示 Y12，13/14 备用 Y11/W11。网络名→封装脚取自核心板原理图第 6 页（pdfplumber 文字错一行，已用现有可用引脚校对）。
  J4.27~36 与摄像头共用，未用。
- **构建坑**：`vivado -log <scratchpad 路径>` 时 create_bd_design 反复报 `couldn't read file ...: No error`（每次文件不同），
  用 `-nolog` 并把输出重定向到 `build/*.out` 就正常。另外 `create_project.tcl` 中新增注释写成了英文。
- **软件**：`traffic_light.c/h`（纯 C 状态机，`sw/test/test_traffic.c` 测试，已并入 gen_test）：
  正常 绿 20s → 黄 3s → 全红 2s 轮换南北/东西；救护车（target.active && direction_valid）方位→进口
  （`TL_NORTH_AZ_DEG`=0，逆时针：90°西、180°南、270°东）→ 相位；来车相位绿灯则保持（绿灯延长），
  否则当前绿灯满 5s 后转黄（红灯早断）→ 全红 → 来车相位绿灯保持；警笛方向消失 3s 后结束优先，再绿 5s 恢复循环；
  优先期间改到另一相位须持续 1s。不变量测试：不会两相位同时非红、绿后必黄、黄≥3s、每进口只亮一灯。
  `oled_tl.c`：OLED 128x64 显示两相位灯色与倒计时（优先保持时 "--"）、底行"正常通行/救护车优先 X口"，
  中文复用 hdmi_font（`hfont_find`/`hfont_next_cp` 已从 hdmi_ui.c 公开），预览 `sw/test/preview_oled_tl.py` → `docs/oled_tl_preview.png`。
  `main.c`：每块 tl_step + XGpio 输出（`TL_OUT_INVERT` 可改低电平有效），串口 `TL,...` 行，HDMI 底栏右侧显示信号灯状态。
  字库扫描文件加入 oled_tl.c、traffic_light.c。
- 2026-10-24 22:30 硬件重建（WNS +11.67ns，io 报告 tl_out 15 路引脚与表一致）→ create_sdk.tcl 重建工作区
  （`XPAR_AXI_GPIO_TL_BASEADDR` 0x41220000，HDMI 仍 0x43C00000）→ BOOT.bin 烧 QSPI → JTAG 运行。
  JTAG 每 3s 读 0x41220000：0x264（南北绿）→0x252（南北黄）→0x249（全红）→0x909（东西绿）→0x489（东西黄）→0x249→0x264，
  周期与设计一致。当时串口线未连接（COM11 消失），`TL,...` 串口行与 OLED/HDMI 画面未在板上看到；救护车优先未在板上实测。
- 旧硬件备份：`backup_build_extras_20261023/system_wrapper_before_tl.hdf`、`hw_platform_before_tl/`。
