# Linux 编译与部署指南

本文档只针对 Linux 环境（Ubuntu 22.04 x86 调试机 / RK3568 ARM64 目标板），
说明依赖安装、命令行 make 编译（不依赖 Qt Creator）、运行与部署。

---

## 1. 需要安装的库

### 1.1 编译机（Ubuntu 22.04 x86）

```bash
# 编译工具链: g++ / make / libc 头文件
sudo apt install build-essential

# Qt 开发包 (qmake + Core/GUI/Widgets 头文件与库)
sudo apt install qtbase5-dev qt5-qmake qtchooser

# Qt SerialPort 模块 (本项目 QT += serialport 必需, 最容易漏装)
sudo apt install libqt5serialport5-dev

# 虚拟串口调试工具
sudo apt install socat

# 中文字体 (界面为中文, 缺字体显示方块)
sudo apt install fonts-noto-cjk
```

> 说明：Ubuntu 22.04 官方源的 Qt 版本是 5.15.3（没有 5.12），
> 与本项目代码完全兼容，调试可直接使用。
> 如需严格 Qt 5.12，参见文末附录。

### 1.2 目标板（RK3568，只运行不编译）

程序动态链接 Qt 时，目标板只需运行库，体积小，适合小内存：

```bash
sudo apt install libqt5widgets5 libqt5gui5 libqt5core5a libqt5serialport5
sudo apt install fonts-noto-cjk
```

### 1.3 串口权限（编译机和目标板都要做）

```bash
# 普通用户访问 /dev/ttyS* 需加入 dialout 组, 否则报 Permission denied
sudo usermod -aG dialout $USER
# 注销并重新登录后生效, 可用 groups 命令确认
```

---

## 2. 不用 Qt Creator，用 make 命令编译

### 2.1 原理说明

Qt 项目不能直接手写 Makefile 编译，原因是 Qt 的信号槽机制依赖
**moc（元对象编译器）** 对含 `Q_OBJECT` 宏的头文件做代码生成。
正确流程是：

```
RS485Control.pro  --(qmake)-->  Makefile  --(make)-->  可执行文件
```

qmake 只是"生成 Makefile"这一步，之后就是纯标准的 make 编译，
不需要打开任何 Qt IDE。生成的 Makefile 已自动包含 moc 规则。

### 2.2 完整编译步骤

```bash
cd ~/RS485Control            # 进入项目根目录 (有 RS485Control.pro 的目录)

qmake RS485Control.pro       # 生成 Makefile (只需在 .pro 变化后重新执行)

make -j$(nproc)              # 并行编译, 产物为 ./RS485Control

# 常用附加命令:
make clean                   # 清理 .o 等中间产物 (保留 Makefile 和可执行文件)
make distclean               # 彻底清理, 包括 Makefile, 需重新 qmake
```

中间产物（moc/obj）统一生成在 `build/` 目录下（.pro 中已配置），
不会污染 `src/` 源码树。

### 2.3 推荐：影子构建（源码目录外编译）

```bash
mkdir -p ~/build-rs485 && cd ~/build-rs485
qmake ~/RS485Control/RS485Control.pro
make -j$(nproc)
```

好处：源码目录保持干净，删掉 build 目录即完全清理。

### 2.4 常见编译报错对照

| 报错 | 原因 | 解决 |
|------|------|------|
| `qmake: command not found` | 未装 qt5-qmake，或系统有多套 Qt | `sudo apt install qt5-qmake qtchooser`；或用完整路径 `/usr/lib/qt5/bin/qmake` |
| `Unknown module(s) in QT: serialport` | 缺 SerialPort 开发包 | `sudo apt install libqt5serialport5-dev` |
| `Project ERROR: Unknown module(s) in QT: widgets` | 缺 Qt 基础开发包 | `sudo apt install qtbase5-dev` |
| `g++: command not found` | 缺编译器 | `sudo apt install build-essential` |

---

## 3. 运行

### 3.1 准备配置文件

程序按以下顺序查找 `app.ini`（见 `src/main.cpp`）：
当前目录 `config/app.ini` → 当前目录 `app.ini` → 可执行文件目录下同名路径。

```bash
cd ~/RS485Control            # 在项目根目录运行, 可直接找到 config/app.ini
./RS485Control               # 影子构建时: 把 config/ 拷到可执行文件旁
```

修改 `config/app.ini` 中的串口设备：

```ini
[Port0]
enabled=true
device=/dev/ttyS1            ; RK3568 实际串口节点; 调试时填 socat 给出的 /dev/pts/N
```

### 3.2 x86 无真实串口时用 socat 虚拟串口对

```bash
socat -d -d pty,raw,echo=0 pty,raw,echo=0
# 输出两行, 如:
#   PTY is /dev/pts/3   -> 填到 app.ini 的 device=
#   PTY is /dev/pts/4   -> 接 Modbus 从站模拟器 (如 diagslave)
```

### 3.3 RK3568 无桌面环境时

Qt 需直接渲染帧缓冲，用平台插件参数启动：

```bash
./RS485Control -platform linuxfb     # 通用帧缓冲
# 或
./RS485Control -platform eglfs       # 有 GPU 驱动时性能更好
```

触摸屏一般由 libinput 自动识别；若触点偏移再考虑 tslib 校准。

### 3.4 数据目录

`app.ini` 中 `dataPath` 指向 CSV 存储目录，RK3568 建议：

```bash
mkdir -p /userdata/logs
# app.ini: dataPath=/userdata/logs
```

---

## 4. 部署到 RK3568

1. 用 ARM 版 Qt 交叉编译（或直接在板上装 `qtbase5-dev` 等本地编译，速度慢但省事）
2. 拷贝三样东西到板上：可执行文件 `RS485Control`、`config/app.ini`、（首次）建好数据目录
3. 按 1.2 节安装运行库，按 1.3 节配置串口权限
4. 开机自启可用 systemd service 或 rc.local，按需添加 `-platform linuxfb`

---

## 附录：严格使用 Qt 5.12

- **x86**：从 <https://download.qt.io/archive/qt/5.12/> 下载
  `qt-opensource-linux-x64-5.12.12.run` 安装，之后用
  `~/Qt5.12.12/5.12.12/gcc_64/bin/qmake` 替代系统 qmake，make 步骤不变。
- **RK3568 (ARM64)**：官方无预编译包，需用 `qt-everywhere-src-5.12.12`
  源码交叉编译；configure 时 `-skip` 掉不用的模块，只保留
  `qtbase + qtserialport`，产物体积小，符合小内存要求。
