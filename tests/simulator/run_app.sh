#!/bin/sh
# 以模拟器运行目录为工作目录启动主程序:
# 只有从 tests/simulator/run 启动, 主程序才会加载指向模拟串口的
# config/app.ini, 数据目录也隔离在 run/data 下, 不影响真实现场数据。
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="$HERE/run"
[ -d "$RUN" ] || { echo "请先启动模拟器: python3 $HERE/pty_modbus_sim.py"; exit 1; }
cd "$RUN" || exit 1
BIN="$HERE/../../RS485Control"
[ -x "$BIN" ] || BIN="RS485Control"

# 从 Snap 版 VS Code/Codex 终端启动时，GTK/GIO 会指向 Snap 自带的旧
# glibc，系统 Qt 程序加载后会发生 GLIBC_PRIVATE 符号冲突。
if [ -n "$SNAP" ]; then
    unset GTK_PATH GTK_EXE_PREFIX GTK_IM_MODULE_FILE GTK_MODULES
    unset GIO_MODULE_DIR SNAP_LIBRARY_PATH LD_LIBRARY_PATH
    QT_ACCESSIBILITY=0
    export QT_ACCESSIBILITY
    if [ -n "$XDG_DATA_DIRS_VSCODE_SNAP_ORIG" ]; then
        XDG_DATA_DIRS="$XDG_DATA_DIRS_VSCODE_SNAP_ORIG"
        export XDG_DATA_DIRS
    fi
fi

exec "$BIN" "$@"
