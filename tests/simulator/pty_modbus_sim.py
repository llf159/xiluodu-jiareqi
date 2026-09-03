#!/usr/bin/env python3
"""溪洛渡下位机 RS485 / Modbus RTU 模拟器 (独立于主程序)。

通过 Linux PTY (伪终端) 模拟两条 RS485 总线及其上的 PCB 板从站，
主程序无需真实硬件即可运行: 启动时自动生成稳定符号链接和一份指向
这些链接的 config/app.ini，把主程序工作目录切到运行目录即可。

协议 (与 src/rs485device.h 一致):
    Modbus RTU 19200/N/8/1, 0x03 读 / 0x06、0x10 写
    0x0001 高压输入 0x0002 备用 0x0003 高压传感器电压(×10)
    0x0004~0x0009 温湿度1~3 温度/湿度(×10, INT16)
    0x000A~0x000B PT100-1/2 温度(×10, INT16)
    0x0011~0x001A OT01~OT10 读写 0/1

物理模型:
    OT3 (低功率) / OT4 (高功率) 打开时板内温度按功率上升, 关闭后向
    环境温度回落; 外部温湿度探头向板内温度趋近并叠加噪声, 湿度随机
    游走。物理时间使用单调时钟，不会因为主站轮询变快而加速升温。

用法:
    python3 pty_modbus_sim.py                    # 两条总线, 默认从站, 自动启动上位机
    python3 pty_modbus_sim.py --devices0 1,2,3 --devices1 4,5
    python3 pty_modbus_sim.py --script cmds.txt  # 启动时先执行命令文件
    python3 pty_modbus_sim.py --no-interactive   # 不读 stdin (自动化, 不启动上位机)
    python3 pty_modbus_sim.py --no-launch-app    # 只跑模拟器, 不自动启动上位机
    python3 pty_modbus_sim.py --real /dev/ttyUSB0 /dev/ttyUSB1
                                                 # 用真实串口替代 PTY
                                                 # (需 pyserial)

交互命令 (运行中输入, P:S 表示 总线:从站, 如 0:2):
    help                          命令列表
    status                        所有从站状态
    add P:S [环境温度]            运行中接入新子站
    remove P:S                    运行中移除子站
    highvoltage P:S on|off        出现/解除高压报警 (数字量和模拟量同步)
    temperature P:S 温度          单独固定三路温湿度温度 ℃
    pt100 P:S 温度               单独固定两路 PT100 温度 ℃
    trend P:S ℃/分钟              持续升温或降温, 0 停止变化
    set P:S 字段 值               固定字段 (见 status), 偏离物理模型
    auto P:S                      清除固定, 恢复自动模拟
    ambient P:S 温度              设环境温度 ℃
    normal P:S                    恢复正常温度和无高压状态
    bus P off|on                  整条总线断开/接通
    scenario normal|highvoltage|temperature|crowded   一键现场场景
    log on|off                    帧日志开关
    quit                          退出

现场场景:
    normal       恢复默认 5 块子站、无高压、22 ℃稳定状态
    highvoltage  所有现有子站同时出现高压报警
    temperature  各子站分别持续升温、降温或保持稳定
    crowded      两条总线各接入 16 块子站，测试发现与轮询压力
"""

from __future__ import annotations

import argparse
import curses
import os
import random
import re
import selectors
import shutil
import signal
import struct
import subprocess
import sys
import termios
import threading
import time

RUN_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run")

AMBIENT_DEFAULT = 22.0
TICK_S = 0.5
OT3_HEAT = 0.030   # ℃/s 低功率回路
OT4_HEAT = 0.060   # ℃/s 高功率回路
COOL_TAU_S = 400.0
PT_BIAS = (-0.15, 0.10)
TH_BIAS = (-0.30, -0.10, 0.20)
TEMP_MIN, TEMP_MAX = -45.0, 90.0
SET_FIELDS = {
    "hv": "hv_input", "hv_input": "hv_input",
    "reserved": "reserved",
    "volt": "voltage", "voltage": "voltage",
    "pt1": "pt1_temp", "pt2": "pt2_temp",
}
for _i in range(1, 4):
    SET_FIELDS[f"th{_i}t"] = f"th{_i}_temp"
    SET_FIELDS[f"th{_i}h"] = f"th{_i}_humi"
    SET_FIELDS[f"th{_i}_temp"] = f"th{_i}_temp"
    SET_FIELDS[f"th{_i}_humi"] = f"th{_i}_humi"


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def rtu(payload: bytes) -> bytes:
    return payload + struct.pack("<H", crc16(payload))


class Device:
    """一块 PCB 板从站: 寄存器、输出状态与温度物理模型。"""

    def __init__(self, slave_id: int, ambient: float):
        self.id = slave_id
        self.ambient = ambient
        self.internal = ambient + random.uniform(-0.5, 0.5)
        self.ot = [0] * 10
        self.regs = {
            "hv_input": 1,                       # 现场低电平触发，1 为正常
            "reserved": 0,
            "voltage": 0,                         # 正常时高压传感器无电压
        }
        for i in range(3):
            self.regs[f"th{i + 1}_temp"] = 0      # 由物理模型填充
            self.regs[f"th{i + 1}_humi"] = 550    # 55.0 %RH
        self.regs["pt1_temp"] = 0
        self.regs["pt2_temp"] = 0
        self.pinned = set()      # set 命令固定、不再被物理模型覆盖的字段
        self.trend_c_per_min = 0.0
        self.tick()

    # ---------- 物理模型 ----------
    def tick(self, elapsed_s: float = TICK_S):
        elapsed_s = max(0.0, elapsed_s)
        heat = self.ot[2] * OT3_HEAT + self.ot[3] * OT4_HEAT
        self.internal += (heat + self.trend_c_per_min / 60.0) * elapsed_s
        self.internal += ((self.ambient - self.internal)
                          * min(1.0, elapsed_s / COOL_TAU_S))
        self.internal = max(TEMP_MIN, min(TEMP_MAX, self.internal))
        if "pt1_temp" not in self.pinned:
            self.regs["pt1_temp"] = self._temp10(self.internal + PT_BIAS[0]
                                                 + random.uniform(-0.02, 0.02))
        if "pt2_temp" not in self.pinned:
            self.regs["pt2_temp"] = self._temp10(self.internal + PT_BIAS[1]
                                                 + random.uniform(-0.02, 0.02))
        for i in range(3):
            key_t, key_h = f"th{i + 1}_temp", f"th{i + 1}_humi"
            if key_t not in self.pinned:
                target = self.internal + TH_BIAS[i] + random.uniform(-0.05, 0.05)
                self.regs[key_t] = self._temp10(target)
            if key_h not in self.pinned:
                # 随机游走按 sqrt(dt) 缩放，避免模拟结果依赖主站轮询频率。
                h = (self.regs[key_h] / 10.0
                     + random.uniform(-0.15, 0.15) * (elapsed_s / TICK_S) ** 0.5)
                self.regs[key_h] = int(round(max(0.0, min(99.9, h)) * 10))

    @staticmethod
    def _temp10(value: float) -> int:
        return struct.unpack("h", struct.pack("H", int(round(
            max(TEMP_MIN, min(TEMP_MAX, value)) * 10) & 0xFFFF)))[0]

    # ---------- 寄存器空间 ----------
    def read_regs(self, start: int, count: int) -> list[int] | None:
        """返回寄存器值列表; 超出映射范围返回 None (地址非法)。"""
        values: list[int] = []
        for addr in range(start, start + count):
            v = self._reg(addr)
            if v is None:
                return None
            values.append(v)
        return values

    def _reg(self, addr: int) -> int | None:
        if 0x0001 <= addr <= 0x000B:
            idx = addr - 0x0001
            keys = ["hv_input", "reserved", "voltage",
                    "th1_temp", "th1_humi", "th2_temp", "th2_humi",
                    "th3_temp", "th3_humi", "pt1_temp", "pt2_temp"]
            return self.regs[keys[idx]] & 0xFFFF
        if 0x0011 <= addr <= 0x001A:
            return self.ot[addr - 0x0011] & 0xFFFF
        return None

    def write_regs(self, start: int, values: list[int]) -> int | None:
        """原子写 OT 区；成功返回 None，失败返回 Modbus 异常码。"""
        if not values or start < 0x0011 or start + len(values) - 1 > 0x001A:
            return 0x02
        if any(value not in (0, 1) for value in values):
            return 0x03
        for i, value in enumerate(values):
            self.ot[start - 0x0011 + i] = value
        return None

    # ---------- 展示 ----------
    def status_line(self, port: int) -> str:
        r = self.regs
        states = []
        if r["hv_input"] == 0 or r["voltage"] > 50:
            states.append("高压")
        if self.trend_c_per_min:
            states.append(f"温度趋势={self.trend_c_per_min:+g}℃/min")
        if self.pinned:
            states.append("固定:" + ",".join(sorted(self.pinned)))
        return (
            f"  P{port}:S{self.id:<3d} hv={r['hv_input']} volt={r['voltage'] / 10:.1f}V "
            f"int={self.internal:5.1f}C amb={self.ambient:5.1f}C "
            f"th=[{r['th1_temp'] / 10:.1f},{r['th2_temp'] / 10:.1f},{r['th3_temp'] / 10:.1f}] "
            f"rh=[{r['th1_humi'] / 10:.0f},{r['th2_humi'] / 10:.0f},{r['th3_humi'] / 10:.0f}] "
            f"pt=[{r['pt1_temp'] / 10:.1f},{r['pt2_temp'] / 10:.1f}] "
            f"OT1-10={''.join(str(x) for x in self.ot)}"
            + ("  状态: " + " ".join(states) if states else "")
        )


class Bus:
    """一条 RS485 总线: PTY (或真实串口) + 若干从站, 独立线程收发。"""

    def __init__(self, index: int, name: str, slave_ids: list[int],
                 transport, sim: "Simulator"):
        self.index = index
        self.name = name
        self.transport = transport        # (fd, close_fn) 或 pyserial 对象
        self.sim = sim
        self.devices = {sid: Device(sid, AMBIENT_DEFAULT) for sid in slave_ids}
        self.bus_off = False
        self.rx = b""
        self.last_rx_at = None
        self.running = True
        self.last_request_slave = None
        self.last_physics_at = time.monotonic()

    def respond(self, frame: bytes):
        """按从站寄存器状态生成并发送响应。"""
        slave_id = frame[0]
        dev = self.devices.get(slave_id)
        tag = f"S{slave_id}"
        if self.bus_off:
            self.sim.log(f"P{self.index} {tag} 无响应 (总线断开)")
            return

        previous_slave = self.last_request_slave
        self.last_request_slave = slave_id
        if dev is None:
            self.sim.log(f"P{self.index} {tag} 无响应 (静默)")
            return

        fc = frame[1]
        if fc in (0x03, 0x04):
            start, count = struct.unpack(">HH", frame[2:6])
            if count < 1 or count > 125 or start + count > 0x10000:
                body = bytes([slave_id, fc | 0x80, 0x03])
            else:
                regs = dev.read_regs(start, count)
                if regs is None:
                    body = bytes([slave_id, fc | 0x80, 0x02])
                else:
                    body = bytes([slave_id, fc, len(regs) * 2]) + struct.pack(
                        f">{len(regs)}H", *regs)
            resp = rtu(body)
        elif fc == 0x06:
            start, value = struct.unpack(">HH", frame[2:6])
            exception = dev.write_regs(start, [value])
            if exception is None:
                resp = rtu(frame[:6])
            else:
                resp = rtu(bytes([slave_id, fc | 0x80, exception]))
        elif fc == 0x10:
            start, count = struct.unpack(">HH", frame[2:6])
            byte_count = frame[6]
            if (count < 1 or count > 123 or byte_count != count * 2
                    or len(frame) != 9 + byte_count):
                resp = rtu(bytes([slave_id, fc | 0x80, 0x03]))
            elif self.sim.emulate_write_sync and previous_slave != slave_id:
                # 现场板卡的已知行为：总线上刚访问过别的地址时，本机第一次
                # 0x10 不处理也不应答；一次同地址读或再次请求后恢复。
                self.sim.log(f"P{self.index} TX {tag} 首次写静默 (等待同地址同步)")
                return
            else:
                values = list(struct.unpack(f">{count}H", frame[7:7 + byte_count]))
                exception = dev.write_regs(start, values)
                if exception is None:
                    resp = rtu(bytes([slave_id, fc]) + struct.pack(">HH", start, count))
                else:
                    resp = rtu(bytes([slave_id, fc | 0x80, exception]))
        else:
            resp = rtu(bytes([slave_id, fc | 0x80, 0x01]))
        self.sim.log(f"P{self.index} TX {tag} {resp.hex(' ')}")
        self.transport.write(resp)

    def handle_rx(self, chunk: bytes):
        now = time.monotonic()
        if (self.rx and self.last_rx_at is not None
                and now - self.last_rx_at >= 0.01):
            self.sim.log(f"P{self.index} RX 帧间静默后清除 {len(self.rx)} 个残留字节")
            self.rx = b""
        self.last_rx_at = now
        self.rx += chunk
        while self.rx:
            frame = self._extract()
            if frame is None:
                return
            slave_id = frame[0]
            self.sim.log(f"P{self.index} RX S{slave_id} {frame.hex(' ')}")
            self.respond(frame)

    def _extract(self) -> bytes | None:
        """按功能码推断帧长, 从缓冲区取一帧; 无完整帧返回 None。"""
        while len(self.rx) >= 2:
            fc = self.rx[1]
            if fc == 0x10:
                if len(self.rx) < 7:
                    return None
                byte_count = self.rx[6]
                if byte_count > 246:
                    self.rx = self.rx[1:]
                    continue
                # 从站+功能码+地址+数量+字节长+数据+CRC
                need = 9 + byte_count
            else:
                need = 8
            if len(self.rx) < need:
                return None
            frame = self.rx[:need]
            if crc16(frame[:-2]) == struct.unpack("<H", frame[-2:])[0]:
                self.rx = self.rx[need:]
                return frame
            self.sim.log(f"P{self.index} RX 请求 CRC/对齐错误, 丢弃字节 "
                         f"0x{self.rx[0]:02X}")
            self.rx = self.rx[1:]
        return None

    def loop(self):
        sel = selectors.DefaultSelector()
        sel.register(self.transport.fileno(), selectors.EVENT_READ)
        while self.running:
            for key, _ in sel.select(timeout=TICK_S):
                chunk = self.transport.read(512)
                if chunk:
                    self.handle_rx(chunk)
                else:
                    time.sleep(0.05)   # 对端关闭: 避免读 EOF 忙转
            now = time.monotonic()
            if now - self.last_physics_at >= TICK_S:
                elapsed_s = now - self.last_physics_at
                self.last_physics_at = now
                with self.sim.lock:
                    for dev in self.devices.values():
                        dev.tick(elapsed_s)
        sel.close()

    def stop(self):
        self.running = False


class PtyTransport:
    """PTY 传输: 主端归模拟器, 从端路径给主程序打开。

    稳定符号链接建在 /tmp/rs485_sim/ 下 (工作区可能是不支持符号链接
    的文件系统); 建立失败时主程序配置直接使用 /dev/pts/N 真实路径。
    """

    LINK_DIR = "/tmp/rs485_sim"

    def __init__(self, index: int, name: str):
        master_fd, slave_fd = os.openpty()
        for fd in (master_fd, slave_fd):
            attrs = termios.tcgetattr(fd)
            attrs[0] = attrs[1] = attrs[3] = 0          # iflag/oflag/lflag: raw
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        self.slave_path = os.ttyname(slave_fd)
        # 保持从端 fd 打开: 否则全部从端关闭后主端 read 会得到 EIO
        self._slave_fd = slave_fd
        self.link = os.path.join(self.LINK_DIR, f"port{index}")
        try:
            os.makedirs(self.LINK_DIR, exist_ok=True)
            try:
                os.unlink(self.link)
            except FileNotFoundError:
                pass
            os.symlink(self.slave_path, self.link)
            self.config_path = self.link
        except OSError:
            self.link = None
            self.config_path = self.slave_path

    def fileno(self):
        return self.master_fd

    def read(self, n):
        try:
            return os.read(self.master_fd, n)
        except OSError:
            return b""   # 对端断开等瞬态错误: 按无数据处理

    def write(self, data):
        os.write(self.master_fd, data)

    def close(self):
        if self.link:
            try:
                os.unlink(self.link)
            except FileNotFoundError:
                pass
        os.close(self.master_fd)
        os.close(self._slave_fd)


class SerialTransport:
    """真实串口传输 (两根 USB-RS485 对拷/自环测试), 需 pyserial。"""

    def __init__(self, device: str):
        import serial  # noqa: 延迟导入, 只有 --real 模式需要
        self.ser = serial.Serial(device, 19200, bytesize=8, parity="N",
                                 stopbits=1, timeout=0)
        self.device = device
        self.config_path = device

    def fileno(self):
        return self.ser.fileno()

    def read(self, n):
        return self.ser.read(n)

    def write(self, data):
        self.ser.write(data)

    def close(self):
        self.ser.close()


class Simulator:
    def __init__(self, args):
        self.lock = threading.RLock()
        self.console_log = True
        self.log_enabled = True
        self.print_lock = threading.Lock()
        self.emulate_write_sync = not args.no_write_sync_quirk
        self.logfile = open(args.logfile, "a", encoding="utf-8") if args.logfile else None
        transports = []
        for i in range(2):
            device = args.real[i] if args.real else None
            transports.append(
                SerialTransport(device) if device else PtyTransport(i, f"port{i}"))
        self.buses = [
            Bus(0, "RS485-A", parse_ids(args.devices0), transports[0], self),
            Bus(1, "RS485-B", parse_ids(args.devices1), transports[1], self),
        ]
        self.threads = [threading.Thread(target=b.loop, daemon=True)
                        for b in self.buses]

    # ---------- 日志 ----------
    def log(self, msg: str):
        if not self.log_enabled:
            return
        line = time.strftime("[%H:%M:%S]") + f"{int(time.time() * 1000) % 1000:03d} " + msg
        with self.print_lock:
            if self.console_log:
                print(line, flush=True)
            if self.logfile:
                self.logfile.write(line + "\n")
                self.logfile.flush()

    # ---------- 命令 ----------
    def execute(self, line: str) -> bool:
        """执行一条交互命令; 返回 False 表示退出。"""
        parts = line.split()
        if not parts:
            return True
        cmd, args = parts[0].lower(), parts[1:]
        with self.lock:
            handler = getattr(self, f"cmd_{cmd}", None)
            if handler:
                try:
                    result = handler(args)
                    return result if result is False else True
                except Exception as exc:      # 交互命令不允许弄崩模拟器
                    self.log(f"命令错误: {exc}")
                    return True
            self.log(f"未知命令: {cmd} (输入 help 查看命令)")
            return True

    def _dev(self, ps: str) -> Device:
        bus, slave_id = self._target(ps)
        dev = bus.devices.get(slave_id)
        if dev is None:
            raise ValueError(f"从站 {ps} 不存在")
        return dev

    def _target(self, ps: str) -> tuple[Bus, int]:
        m = re.fullmatch(r"(\d+):(\d+)", ps)
        if not m:
            raise ValueError(f"目标格式应为 总线:从站, 如 0:2, 收到 {ps!r}")
        port, slave_id = int(m.group(1)), int(m.group(2))
        if not 0 <= port < len(self.buses):
            raise ValueError(f"总线编号必须为 0~{len(self.buses) - 1}")
        if not 1 <= slave_id <= 247:
            raise ValueError("从站地址必须为 1~247")
        return self.buses[port], slave_id

    def cmd_help(self, _args):
        print("交互命令" + __doc__.split("交互命令", 1)[1], flush=True)
        return True

    def cmd_status(self, _args):
        for bus in self.buses:
            state = "总线静默(拔线)" if bus.bus_off else "正常"
            print(f"P{bus.index} {bus.name} [{state}] 从站: {sorted(bus.devices)}",
                  flush=True)
            for dev in sorted(bus.devices.values(), key=lambda d: d.id):
                print(dev.status_line(bus.index), flush=True)
        return True

    def cmd_set(self, args):
        if len(args) < 3:
            raise ValueError("用法: set P:S 字段 值")
        dev = self._dev(args[0])
        field, value = SET_FIELDS.get(args[1].lower()), float(args[2])
        if field is None:
            raise ValueError(f"未知字段 {args[1]}, 可用: {', '.join(sorted(SET_FIELDS))}")
        if field == "voltage":
            if not 0 <= value <= 6553.5:
                raise ValueError("电压必须为 0~6553.5 V")
            dev.regs[field] = int(round(value * 10))
        elif field in ("hv_input", "reserved"):
            if value not in (0, 1):
                raise ValueError("数字量输入只能设为 0 或 1")
            dev.regs[field] = int(value)
        elif field in ("pt1_temp", "pt2_temp") or field.endswith("_temp"):
            raw = int(round(value * 10))
            if not -32768 <= raw <= 32767:
                raise ValueError("温度放大 10 倍后必须在 INT16 可表示范围内")
            dev.regs[field] = raw
        else:
            if not 0 <= value <= 6553.5:
                raise ValueError("湿度原始值必须在 UINT16 可表示范围内")
            dev.regs[field] = int(round(value * 10))
        dev.pinned.add(field)
        self.log(f"{args[0]} {field} = {value} (已固定, auto 恢复)")

    def cmd_auto(self, args):
        dev = self._dev(args[0])
        dev.pinned.clear()
        self.log(f"{args[0]} 已恢复自动模拟")

    def cmd_ambient(self, args):
        self._dev(args[0]).ambient = float(args[1])

    def cmd_add(self, args):
        bus, slave_id = self._target(args[0])
        if slave_id in bus.devices:
            raise ValueError(f"从站 {args[0]} 已存在")
        ambient = float(args[1]) if len(args) > 1 else AMBIENT_DEFAULT
        bus.devices[slave_id] = Device(slave_id, ambient)
        self.log(f"{args[0]} 已接入, 环境温度 {ambient:.1f} ℃")

    def cmd_remove(self, args):
        bus, slave_id = self._target(args[0])
        if bus.devices.pop(slave_id, None) is None:
            raise ValueError(f"从站 {args[0]} 不存在")
        self.log(f"{args[0]} 已断开并移除")

    def cmd_highvoltage(self, args):
        dev = self._dev(args[0])
        active = args[1].lower() in ("on", "1", "true", "yes")
        # 默认数字量模式为低电平触发；模拟量模式严格高于 5 V 触发。
        dev.regs["hv_input"] = 0 if active else 1
        dev.regs["voltage"] = 120 if active else 0
        self.log(f"{args[0]} 高压报警 = {'出现' if active else '解除'}")

    cmd_hv = cmd_highvoltage

    def cmd_temperature(self, args):
        dev = self._dev(args[0])
        value = float(args[1])
        if not TEMP_MIN <= value <= TEMP_MAX:
            raise ValueError(f"模拟温度必须为 {TEMP_MIN:g}~{TEMP_MAX:g} ℃")
        for index in range(1, 4):
            field = f"th{index}_temp"
            dev.regs[field] = dev._temp10(value)
            dev.pinned.add(field)
        self.log(f"{args[0]} 温湿度温度固定为 {value:.1f} ℃")

    cmd_temp = cmd_temperature

    def cmd_pt100(self, args):
        dev = self._dev(args[0])
        value = float(args[1])
        if not TEMP_MIN <= value <= TEMP_MAX:
            raise ValueError(f"模拟温度必须为 {TEMP_MIN:g}~{TEMP_MAX:g} ℃")
        for index in range(1, 3):
            field = f"pt{index}_temp"
            dev.regs[field] = dev._temp10(value)
            dev.pinned.add(field)
        self.log(f"{args[0]} PT100 温度固定为 {value:.1f} ℃")

    cmd_pt = cmd_pt100

    def cmd_trend(self, args):
        dev = self._dev(args[0])
        dev.trend_c_per_min = float(args[1])
        self.log(f"{args[0]} 温度趋势 = {dev.trend_c_per_min:+g} ℃/min")

    def cmd_normal(self, args):
        dev = self._dev(args[0])
        dev.pinned.clear()
        dev.ambient = AMBIENT_DEFAULT
        dev.internal = AMBIENT_DEFAULT
        dev.trend_c_per_min = 0
        dev.regs["hv_input"] = 1
        dev.regs["voltage"] = 0
        dev.tick(0)
        self.log(f"{args[0]} 已恢复正常")

    def cmd_bus(self, args):
        bus = self.buses[int(args[0])]
        bus.bus_off = args[1].lower() == "off"
        self.log(f"P{bus.index} 整线静默 = {bus.bus_off}")

    def cmd_scenario(self, args):
        name = args[0].lower()
        if name == "normal":
            for bus, slave_ids in zip(self.buses, ([1, 2, 3], [4, 5])):
                bus.devices = {
                    slave_id: (bus.devices[slave_id] if slave_id in bus.devices
                               else Device(slave_id, AMBIENT_DEFAULT))
                    for slave_id in slave_ids
                }
        elif name == "crowded":
            for bus in self.buses:
                bus.devices = {
                    slave_id: (bus.devices[slave_id] if slave_id in bus.devices
                               else Device(slave_id, AMBIENT_DEFAULT))
                    for slave_id in range(1, 17)
                }
        elif name not in ("highvoltage", "temperature"):
            raise ValueError(
                f"未知场景 {name}, 可用: normal/highvoltage/temperature/crowded")

        for bus in self.buses:
            for dev in bus.devices.values():
                self.cmd_normal([f"{bus.index}:{dev.id}"])

        if name == "highvoltage":
            for bus in self.buses:
                for dev in bus.devices.values():
                    self.cmd_highvoltage([f"{bus.index}:{dev.id}", "on"])
        elif name == "temperature":
            devices = [dev for bus in self.buses for dev in bus.devices.values()]
            for index, dev in enumerate(devices):
                dev.trend_c_per_min = (3.0, -3.0, 0.0)[index % 3]
        self.log(f"场景 {name} 已应用")

    def cmd_log(self, args):
        self.log_enabled = args[0].lower() != "off"

    def cmd_quit(self, _args):
        return False

    cmd_exit = cmd_quit

    # ---------- 生命周期 ----------
    def start(self):
        for t in self.threads:
            t.start()

    def stop(self):
        for bus in self.buses:
            bus.stop()
        for t in self.threads:
            t.join(timeout=2)
        for bus in self.buses:
            bus.transport.close()


class Tui:
    """面向现场场景的全屏控制台。"""

    def __init__(self, stdscr, sim: Simulator):
        self.stdscr = stdscr
        self.sim = sim
        self.selection = 0            # 展平后的从站序号
        self.message = "上下选择，左右调温湿度，,/. 调 PT100；h 高压"
        sim.console_log = False       # 全屏模式下帧日志只写文件
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_RED, -1)     # 高压报警/总线断开
        curses.init_pair(2, curses.COLOR_CYAN, -1)    # 标题/分隔
        curses.init_pair(3, curses.COLOR_GREEN, -1)   # 正常
        curses.init_pair(4, curses.COLOR_YELLOW, -1)  # 选中
        curses.init_pair(5, curses.COLOR_WHITE, curses.COLOR_BLUE)  # 反显选中
        self.stdscr.nodelay(True)
        self.stdscr.keypad(True)
        curses.curs_set(0)

    # ---------- 布局辅助 ----------
    def _flat(self):
        items = []
        for bus in self.sim.buses:
            for dev in sorted(bus.devices.values(), key=lambda d: d.id):
                items.append((bus, dev))
        return items

    def _put(self, y, x, text, attr=0):
        h, w = self.stdscr.getmaxyx()
        if 0 <= y < h and x < w:
            try:
                self.stdscr.addnstr(y, x, text, max(0, w - x - 1), attr)
            except curses.error:
                pass

    # ---------- 绘制 ----------
    def draw(self):
        self.stdscr.erase()
        sim = self.sim
        self._put(0, 0, "下位机模拟器控制台", curses.color_pair(2) | curses.A_BOLD)
        self._put(0, 22, f"P0={len(sim.buses[0].devices)} 块  "
                         f"P1={len(sim.buses[1].devices)} 块")
        self._put(1, 0, "场景: [1]正常5块 [2]全部高压 [3]温度升降 "
                        "[4]满载32块   [q]退出", curses.color_pair(2))
        port_text = "  ".join(
            f"P{bus.index}:{'断开' if bus.bus_off else '接通'} {bus.transport.config_path}"
            for bus in sim.buses)
        self._put(2, 0, port_text, curses.A_DIM)

        flat = self._flat()
        if flat:
            self.selection %= len(flat)
        h, _ = self.stdscr.getmaxyx()
        visible_rows = max(1, h - 7)
        start = max(0, min(self.selection - visible_rows // 2,
                           len(flat) - visible_rows))
        self._put(3, 0, "   子站     外部温度       PT100       环境/趋势       高压    OT1~10",
                  curses.color_pair(2) | curses.A_BOLD)
        for row, (bus, dev) in enumerate(flat[start:start + visible_rows], start=4):
            self.draw_device(row, bus, dev, flat[self.selection] == (bus, dev))

        self._put(h - 2, 0, "[↑↓]选择 [←→]温湿度±1℃ [,/.]PT100±1℃ [s/p]设温 [v]升/降温 "
                            "[h]高压 [a/r]增/减子站 [b]总线 [n]恢复",
                  curses.A_DIM)
        self._put(h - 1, 0, self.message, curses.color_pair(4) | curses.A_BOLD)
        self.stdscr.refresh()

    def draw_device(self, row, bus, dev, selected):
        r = dev.regs
        average = sum(r[f"th{i}_temp"] for i in range(1, 4)) / 30.0
        high_voltage = r["hv_input"] == 0 or r["voltage"] > 50
        trend = f"{dev.trend_c_per_min:+g}℃/min" if dev.trend_c_per_min else "稳定"
        summary = (f"P{bus.index}:S{dev.id:<3d}  {average:5.1f}℃   "
                   f"{r['pt1_temp'] / 10:5.1f}/{r['pt2_temp'] / 10:5.1f}℃   "
                   f"{dev.ambient:5.1f}℃ {trend:>9}   "
                   f"{'报警' if high_voltage else '正常':4}   "
                   f"{''.join(str(x) for x in dev.ot)}")
        marker = ">" if selected else " "
        attr = curses.color_pair(5) if selected else 0
        if high_voltage and not selected:
            attr = curses.color_pair(1) | curses.A_BOLD
        self._put(row, 0, f"{marker} {summary}", attr)

    # ---------- 交互 ----------
    def handle_key(self, key) -> bool:
        """处理按键; 返回 False 退出。"""
        sim = self.sim
        flat = self._flat()
        if not flat:
            if key == ord("1"):
                sim.execute("scenario normal")
            elif key == ord("4"):
                sim.execute("scenario crowded")
            elif key == ord("q"):
                return False
            return True
        self.selection %= len(flat)
        bus, dev = flat[self.selection]
        ps = f"{bus.index}:{dev.id}"

        if key in (curses.KEY_UP, ord("k")):
            self.selection = (self.selection - 1) % len(flat)
        elif key in (curses.KEY_DOWN, ord("j")):
            self.selection = (self.selection + 1) % len(flat)
        elif key == ord("q"):
            return False
        elif key in (curses.KEY_LEFT, ord("[")):
            value = sum(dev.regs[f"th{i}_temp"] for i in range(1, 4)) / 30.0
            sim.execute(f"temperature {ps} {max(TEMP_MIN, value - 1)}")
            self.message = f"{ps} 温湿度温度降低 1 ℃"
        elif key in (curses.KEY_RIGHT, ord("]")):
            value = sum(dev.regs[f"th{i}_temp"] for i in range(1, 4)) / 30.0
            sim.execute(f"temperature {ps} {min(TEMP_MAX, value + 1)}")
            self.message = f"{ps} 温湿度温度升高 1 ℃"
        elif key == ord(","):
            value = sum(dev.regs[f"pt{i}_temp"] for i in range(1, 3)) / 20.0
            sim.execute(f"pt100 {ps} {max(TEMP_MIN, value - 1)}")
            self.message = f"{ps} PT100 温度降低 1 ℃"
        elif key == ord("."):
            value = sum(dev.regs[f"pt{i}_temp"] for i in range(1, 3)) / 20.0
            sim.execute(f"pt100 {ps} {min(TEMP_MAX, value + 1)}")
            self.message = f"{ps} PT100 温度升高 1 ℃"
        elif key == ord("h"):
            active = dev.regs["hv_input"] == 0 or dev.regs["voltage"] > 50
            sim.execute(f"highvoltage {ps} {'off' if active else 'on'}")
            self.message = f"{ps} 高压报警已{'解除' if active else '出现'}"
        elif key == ord("v"):
            rates = (0.0, 3.0, -3.0)
            try:
                rate = rates[(rates.index(dev.trend_c_per_min) + 1) % len(rates)]
            except ValueError:
                rate = 0.0
            sim.execute(f"trend {ps} {rate}")
            self.message = f"{ps} 温度趋势 {rate:+g} ℃/min"
        elif key == ord("a"):
            if len(bus.devices) >= 16:
                self.message = f"P{bus.index} 已达到上位机上限 16 块"
            else:
                slave_id = next(i for i in range(1, 248) if i not in bus.devices)
                sim.execute(f"add {bus.index}:{slave_id}")
                self.message = f"P{bus.index}:S{slave_id} 已接入，等待上位机发现"
        elif key == ord("r"):
            sim.execute(f"remove {ps}")
            self.selection = min(self.selection, max(0, len(flat) - 2))
            self.message = f"{ps} 已移除"
        elif key == ord("n"):
            sim.execute(f"normal {ps}")
            self.message = f"{ps} 已恢复正常"
        elif key == ord("b"):
            sim.execute(f"bus {bus.index} {'on' if bus.bus_off else 'off'}")
            self.message = f"P{bus.index} 总线 {'接通' if not bus.bus_off else '断开'}"
        elif key == ord("s"):
            self.prompt_temperature(ps, "temperature", "温湿度")
        elif key == ord("p"):
            self.prompt_temperature(ps, "pt100", "PT100")
        elif key in map(ord, "1234"):
            name = "normal highvoltage temperature crowded".split()[key - ord("1")]
            sim.execute(f"scenario {name}")
            self.selection = 0
            self.message = f"现场场景 {name} 已切换"
        return True

    def prompt_temperature(self, ps, command, label):
        curses.curs_set(1)
        curses.echo()
        h, _ = self.stdscr.getmaxyx()
        prompt = f"设定 {ps} {label} 温度 ℃: "
        self._put(h - 1, 0, prompt)
        self.stdscr.refresh()
        try:
            input_y, input_x = self.stdscr.getyx()
            raw = self.stdscr.getstr(input_y, input_x, 20).decode().strip()
        except curses.error:
            raw = ""
        curses.noecho()
        curses.curs_set(0)
        if raw:
            self.sim.execute(f"{command} {ps} {raw}")
            self.message = f"{ps} {label} 温度已设为 {raw} ℃"

    def loop(self):
        while True:
            self.draw()
            key = self.stdscr.getch()
            if key == -1:
                time.sleep(0.05)
                continue
            if not self.handle_key(key):
                return


def parse_ids(text: str) -> list[int]:
    ids = []
    for part in text.split(","):
        part = part.strip()
        if part:
            sid = int(part)
            if not 1 <= sid <= 247:
                raise ValueError(f"从站地址 {sid} 超出 1~247")
            if sid in ids:
                raise ValueError(f"从站地址 {sid} 重复")
            ids.append(sid)
    return ids


def write_run_config(port0: str, port1: str):
    """生成指向模拟串口的 config/app.ini, 与主程序配置键保持一致。"""
    cfg_dir = os.path.join(RUN_DIR, "config")
    os.makedirs(cfg_dir, exist_ok=True)
    template = f"""; 模拟器自动生成: 端口指向 PTY 符号链接, 数据目录隔离在 run/ 下
[General]
dataPath=data/logs
displayTheme=standard
highVoltageDetectionMode=analog
highVoltageDigitalTrigger=0
highVoltageThreshold=5
interSlaveDelayMs=50
maxStorageMB=12288
modbusTimeoutMs=500
pollIntervalMs=1000
relaySwitchIntervalSec=10
reservedInputMode=monitor
temperatureControlMode=threshold
temperatureTarget=25
temperatureTargetSource=pt100

[Port0]
baudRate=19200
dataBits=8
device={port0}
enabled=true
frameDelayMs=5
name=RS485-A
parity=N
stopBits=1

[Port1]
baudRate=19200
dataBits=8
device={port1}
enabled=true
frameDelayMs=5
name=RS485-B
parity=N
stopBits=1
"""
    path = os.path.join(cfg_dir, "app.ini")
    with open(path, "w", encoding="utf-8") as f:
        f.write(template)
    return path


def launch_host_app(launcher: str) -> subprocess.Popen | None:
    """通过 run_app.sh 启动上位机, 让模拟器一条命令完成整套联调环境。"""
    repo_bin = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "RS485Control")
    if not (os.path.isfile(repo_bin) and os.access(repo_bin, os.X_OK)) \
            and shutil.which("RS485Control") is None:
        print("  未找到 RS485Control 可执行文件, 请先在仓库根目录执行 make")
        return None
    proc = subprocess.Popen([launcher])
    print(f"  已自动启动上位机 (PID {proc.pid}), 退出模拟器时会一并关闭")
    return proc


def stop_host_app(proc: subprocess.Popen | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--devices0", default="1,2,3",
                        help="总线 0 从站地址, 逗号分隔 (默认 1,2,3)")
    parser.add_argument("--devices1", default="4,5",
                        help="总线 1 从站地址, 逗号分隔 (默认 4,5)")
    parser.add_argument("--script", help="启动时执行的命令文件 (每行一条)")
    parser.add_argument("--no-interactive", action="store_true",
                        help="不读 stdin, 纯后台运行 (自动化)")
    parser.add_argument("--launch-app", dest="launch_app", action="store_true",
                        default=None,
                        help="启动后自动运行上位机 (默认交互模式自动启动)")
    parser.add_argument("--no-launch-app", dest="launch_app",
                        action="store_false",
                        help="不自动启动上位机")
    parser.add_argument("--cli", action="store_true",
                        help="用命令行交互, 不启动全屏控制台")
    parser.add_argument("--real", nargs=2, metavar=("DEV0", "DEV1"),
                        help="用真实串口替代 PTY (需 pyserial)")
    parser.add_argument("--no-write-sync-quirk", action="store_true",
                        help="关闭现场板卡切换地址后首次 0x10 静默行为")
    parser.add_argument("--logfile", default=os.path.join(RUN_DIR, "sim.log"))
    parser.add_argument("--quiet", action="store_true", help="不在控制台打印帧日志")
    args = parser.parse_args()

    # 后台/服务方式运行时 SIGINT 可能被继承为 SIG_IGN (shell 关闭作业控制
    # 时对后台任务的行为), SIGTERM 则是自动化常用的停止方式; 两者都统一
    # 转成 KeyboardInterrupt, 保证 finally 里能一并关闭自动启动的上位机。
    def _request_exit(signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, _request_exit)
    signal.signal(signal.SIGTERM, _request_exit)

    # 默认交互模式自动启动上位机; --no-interactive 自动化默认不启动,
    # 两者都可用 --launch-app / --no-launch-app 显式覆盖。
    launch_app = args.launch_app
    if launch_app is None:
        launch_app = not args.no_interactive

    os.makedirs(RUN_DIR, exist_ok=True)
    sim = Simulator(args)
    if args.quiet:
        sim.console_log = False
    if args.real:
        link0, link1 = args.real
    else:
        link0 = sim.buses[0].transport.config_path
        link1 = sim.buses[1].transport.config_path
    cfg = write_run_config(link0, link1)

    launcher = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_app.sh")
    print("=" * 64)
    print("下位机模拟器已启动 (Ctrl+C 或 q 退出)")
    if not args.real:
        t0, t1 = sim.buses[0].transport, sim.buses[1].transport
        print(f"  总线 0 (RS485-A): {link0} -> {t0.slave_path}")
        print(f"  总线 1 (RS485-B): {link1} -> {t1.slave_path}")
    else:
        print(f"  串口 0 (RS485-A): {link0}")
        print(f"  串口 1 (RS485-B): {link1}")
    print(f"  从站: P0 {parse_ids(args.devices0)} / P1 {parse_ids(args.devices1)}")
    print(f"  已生成主程序配置: {cfg}")
    if launch_app is None:
        print("  启动主程序: python3 " + launcher
              + "   (或去掉 --no-launch-app 自动启动)")
    print("  全屏控制台: 左右调温湿度, ,/. 调 PT100, h 高压, a/r 增减子站")
    print("  命令行模式 (--cli): 输入 help 查看命令")
    print("=" * 64, flush=True)

    if args.script:
        with open(args.script, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    sim.execute(line)

    sim.start()

    app_proc = None
    if launch_app:
        print(f"  正在启动上位机: {launcher}", flush=True)
        app_proc = launch_host_app(launcher)

    use_tui = (not args.cli and not args.no_interactive
               and sys.stdin.isatty() and sys.stdout.isatty())
    try:
        if use_tui:
            curses.wrapper(lambda stdscr: Tui(stdscr, sim).loop())
        elif not args.no_interactive:
            # 管道/文件重定向同样支持, 便于自动化测试
            while True:
                line = sys.stdin.readline()
                if not line:
                    break
                line = line.split("#", 1)[0].strip()
                if line and not sim.execute(line):
                    break
        else:
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        stop_host_app(app_proc)

    sim.stop()
    print("模拟器已退出", flush=True)


if __name__ == "__main__":
    main()
