# ============================================================
# RS485Control - Qt 5.12 qmake 工程
# 目标平台: x86 (Windows/Linux 调试) / RK3568 (Ubuntu 22.04 ARM64)
# 依赖模块: Qt Widgets + SerialPort
# ============================================================

QT += core gui widgets serialport

TARGET = RS485Control
TEMPLATE = app

CONFIG += c++11

# 源码内部以 src/ 为根做相对包含 (如 core/config/appconfig.h)
INCLUDEPATH += src

DEFINES += QT_DEPRECATED_WARNINGS

# 三层结构:
#   appui       - 界面交互 (Qt Widgets)
#   applogic    - 业务逻辑控制 (配置/设备状态/调度/存储)
#   rs485device - RS485 与设备交互 (Modbus RTU / 串口线程)
SOURCES += \
    src/main.cpp \
    src/appui.cpp \
    src/applogic.cpp \
    src/rs485device.cpp

HEADERS += \
    src/appui.h \
    src/applogic.h \
    src/rs485device.h

# 中间产物统一放 build 目录，避免污染源码树
MOC_DIR     = build/moc
OBJECTS_DIR = build/obj
RCC_DIR     = build/rcc
UI_DIR      = build/ui

# 构建后将配置文件随可执行程序发布 (可选, 手动拷贝亦可)
# Linux 部署示例: 可执行文件与 config/app.ini 同级
