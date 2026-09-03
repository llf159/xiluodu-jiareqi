# RS485Control 环境控制上位机

溪洛渡工程现场的环境控制上位机：通过两条 RS485 总线（Modbus RTU，19200/N/8/1）
轮询最多 2×16 块子板，完成温湿度/PT100 数据采集、按子站独立的自动温控、
手动输出控制、高压安全联锁与 CSV 数据记录。目标平台为 RK3568（ARM64）触屏
一体机，开发调试环境为 Ubuntu x86 + Qt 5.15。

## 功能概览

- **子板总览**：全部已发现子板的状态卡片，强调显示每块子板的运行模式
  （● 自动温控 / ● 手动）、当前档位和 OT3/OT4 输出状态，点击进入控制页。
- **子板控制**：按子站独立启停自动温控；OT3/OT4 手动开关；OT1/OT2/OT5/OT6
  备用输出可配置为安全关闭、手动、跟随自动、跟随报警、跟随 OT3/OT4；
  ot07~ot10 指示灯/蜂鸣器按子板状态自动点亮。
- **三种温控模式**：阈值三档、PID 三档、防凝露（露点余量）三档，档位顺序、
  输出点与回差均可在参数页调整；目标温度可跟随子板内部 PT100 平均值或固定值。
- **安全联锁**：高压检测支持数字量（0x0001）与模拟量（0x0003，电压阈值）两种
  来源，触发后切断全部 OT3/OT4 并声光报警；备用输入 0x0002 可配置联锁；
  温湿度自检只对固定极端量程（-40~85 ℃ / 0~100 %RH）报警，不阻塞控制。
- **数据记录与浏览**：跟随轮询周期（1~3600 秒可配）写按日期分文件的 CSV；
  历史页支持日期/子板筛选、曲线与明细联动、秒级聚合与跨主题配色。
- **运维**：存储容量统计、按截止日期预览+二次确认清理、日志容量上限；
  标准/低光/强光三套现场主题，安全语义颜色不随主题变化。

## 目录结构

```
src/                      上位机源码 (C++11 / Qt Widgets)
  rs485device.*           串口与 Modbus RTU 协议层 (SerialPortWorker)
  applogic.*              DeviceManager / PollScheduler / DataLogger /
                          HistoryQuery / StorageRotator / AppConfig
  controlalgorithm.*      温控算法（阈值/PID/防凝露/露点计算/传感器自检）
  appui.*                 全部界面（总览、控制、参数、历史、主题）
  main.cpp                入口
tests/
  controlalgorithm_test.* 温控算法 QtTest 单元测试
  simulator/
    pty_modbus_sim.py     无硬件下位机模拟器（PTY 模拟两条 RS485 总线,
                          默认自动拉起上位机）
    test_pty_modbus_sim.py  模拟器协议与物理模型单元测试
docs/                     设计文档（见下）
config/app.ini            配置文件示例
rs485_modbus_debug.py     独立的 RS485/Modbus 调试脚本（用法见《使用说明.txt》）
RS485Control.pro          qmake 工程文件
上位机软件需求 20260827.md   需求文档
```

## 编译与运行

依赖（Ubuntu 22.04）：`build-essential qtbase5-dev libqt5serialport5-dev`
中文字体与 RK3568 交叉部署细节见 [docs/BUILD_LINUX.md](docs/BUILD_LINUX.md)。

```bash
qmake RS485Control.pro
make -j4
```

运行时从**当前工作目录**加载 `app.ini`（串口、轮询周期、数据路径等），
未配置时使用默认串口并在界面提示。子板不依赖配置文件，启动后自动扫描
Modbus ID 1~247。

## 无硬件联调（推荐）

模拟器用 Linux PTY 模拟两条 RS485 总线和物理温升模型，一条命令拉起
模拟器 + 上位机：

```bash
python3 tests/simulator/pty_modbus_sim.py
```

退出模拟器时自动关闭上位机。常用参数：`--no-launch-app` 只跑模拟器、
`--no-interactive` 自动化后台模式、`--cli` 命令行交互、
`--real /dev/ttyUSB0 /dev/ttyUSB1` 接真实串口。交互控制台支持增减子站、
高压注入、温度调整和 `normal/highvoltage/temperature/crowded` 一键场景。

## 测试

```bash
# 温控算法单元测试 (QtTest)
qmake -o Makefile.controlalgorithm_test tests/controlalgorithm_test.pro
make -f Makefile.controlalgorithm_test && ./controlalgorithm_test

# 模拟器协议与物理模型测试 (纯标准库)
python3 tests/simulator/test_pty_modbus_sim.py
```

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/BUILD_LINUX.md](docs/BUILD_LINUX.md) | 依赖安装、命令行编译、RK3568 部署 |
| [docs/CONTROL_AND_SAFETY_DESIGN.md](docs/CONTROL_AND_SAFETY_DESIGN.md) | 自检、温控、联锁与备用引脚设计 |
| [docs/DATA_BROWSER_DESIGN.md](docs/DATA_BROWSER_DESIGN.md) | 数据浏览交互与聚合规则 |
| [使用说明.txt](使用说明.txt) | `rs485_modbus_debug.py` 调试脚本用法 |
| [AGENTS.md](AGENTS.md) | 工程取舍与参数决策记录（面向后续维护） |

> 注意：高压模拟量阈值等安全参数默认值仅供开发，正式使用前需按现场
> 传感器标定。
