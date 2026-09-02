#!/usr/bin/env python3
"""溪洛渡下位机 RS485 / Modbus RTU 调试工具。

默认串口参数来自《触摸屏与PCB板通迅地址定义》：19200, 8N1。
除 write 子命令外，所有命令都只发送 0x03 读保持寄存器请求。

Linux 常用示例：
    python3 rs485_modbus_debug.py ports
    python3 rs485_modbus_debug.py scan
    python3 rs485_modbus_debug.py snapshot
    python3 rs485_modbus_debug.py read 0x0021 5
    python3 rs485_modbus_debug.py monitor 0x0041 2 --interval 1

Windows 常用示例：
    py rs485_modbus_debug.py ports
    py rs485_modbus_debug.py --port COM3 scan
    py rs485_modbus_debug.py --port COM3 snapshot

写操作有意设置了双重确认。连接真实负载时，先确认加热器、高压和继电器
动作是安全的，再参考 ``python3 rs485_modbus_debug.py write --help``。
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from typing import Callable, Iterable, Sequence

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - 只在依赖缺失时执行
    install_command = (
        "py -m pip install pyserial"
        if sys.platform.startswith("win")
        else "python3 -m pip install pyserial"
    )
    raise SystemExit(
        f"缺少 pyserial，请先执行：{install_command}"
    ) from exc


LINUX_FALLBACK_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 19200
DEFAULT_SLAVE = 1


class ModbusError(Exception):
    """Modbus 调试错误。"""


class ModbusTimeout(ModbusError):
    """从站没有在指定时间内响应。"""


class ModbusProtocolError(ModbusError):
    """响应帧格式或 CRC 不正确。"""


def is_ch34x_port(item: object) -> bool:
    """判断 pyserial 端口信息是否像 CH340/CH341 USB 串口。"""
    vid = getattr(item, "vid", None)
    if vid == 0x1A86:
        return True
    searchable = " ".join(
        str(getattr(item, attribute, "") or "")
        for attribute in ("description", "manufacturer", "product", "hwid")
    ).lower()
    return any(hint in searchable for hint in ("ch340", "ch341", "usb2.0-serial"))


def describe_port(item: object) -> str:
    """生成同时适用于 Linux 和 Windows 的串口描述。"""
    device = str(getattr(item, "device", "未知端口"))
    description = str(getattr(item, "description", "") or "未知设备")
    vid = getattr(item, "vid", None)
    pid = getattr(item, "pid", None)
    serial_number = getattr(item, "serial_number", None)
    fields = [f"{device}: {description}"]
    if vid is not None and pid is not None:
        fields.append(f"VID:PID={vid:04X}:{pid:04X}")
    if serial_number:
        fields.append(f"SN={serial_number}")
    return " | ".join(fields)


def choose_automatic_port(ports: Sequence[object], platform_name: str) -> str:
    """安全地选择串口；有歧义时要求用户明确指定 --port。"""
    ch34x_ports = [item for item in ports if is_ch34x_port(item)]
    if len(ch34x_ports) == 1:
        return str(getattr(ch34x_ports[0], "device"))
    if len(ch34x_ports) > 1:
        choices = "\n".join(f"  {describe_port(item)}" for item in ch34x_ports)
        raise ModbusError(
            "发现多个 CH340/CH341 串口，无法判断哪一个连接机器。"
            "请用 --port 明确指定：\n" + choices
        )

    if platform_name.startswith("win"):
        usb_ports = [item for item in ports if getattr(item, "vid", None) is not None]
        if len(usb_ports) == 1:
            return str(getattr(usb_ports[0], "device"))
        choices = (
            "\n".join(f"  {describe_port(item)}" for item in ports)
            if ports
            else "  （没有发现串口）"
        )
        raise ModbusError(
            "Windows 下未能自动确定 USB-RS485 串口。先运行 ports，"
            "再用 --port COMx 指定，例如 --port COM3。\n当前端口：\n" + choices
        )

    # Linux 沙箱或权限受限时，list_ports 可能看不到设备；保留项目原默认值。
    return LINUX_FALLBACK_PORT


def resolve_serial_port(requested_port: str | None) -> str:
    if requested_port:
        return requested_port
    return choose_automatic_port(list(list_ports.comports()), sys.platform)


EXCEPTION_NAMES = {
    1: "非法功能码",
    2: "非法数据地址",
    3: "非法数据值",
    4: "从站设备故障",
    5: "确认",
    6: "从站设备忙",
    8: "存储奇偶校验错误",
    10: "网关路径不可用",
    11: "网关目标设备无响应",
}


class ModbusExceptionResponse(ModbusError):
    def __init__(self, code: int) -> None:
        self.code = code
        super().__init__(f"从站异常 {code}: {EXCEPTION_NAMES.get(code, '未知异常')}")


def parse_number(text: str) -> int:
    """解析十进制或 0x 开头的十六进制整数。"""
    value_text = text.strip().lower()
    base = 16 if value_text.startswith("0x") else 10
    try:
        return int(value_text, base)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"不是有效整数：{text}") from exc


def crc16_modbus(data: bytes) -> int:
    """计算 Modbus RTU CRC16，返回主机整数。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def add_crc(data: bytes) -> bytes:
    crc = crc16_modbus(data)
    return data + bytes((crc & 0xFF, crc >> 8))


def hex_frame(data: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in data)


class ModbusRTUMaster:
    """很小的 Modbus RTU 主站，只实现本项目需要的 0x03 和 0x10。"""

    def __init__(
        self,
        port: str,
        baudrate: int = DEFAULT_BAUD,
        timeout: float = 0.35,
        verbose: bool = False,
    ) -> None:
        self.port_name = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.verbose = verbose
        self.port: serial.Serial | None = None

    def __enter__(self) -> "ModbusRTUMaster":
        try:
            self.port = serial.Serial(
                port=self.port_name,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=min(0.02, self.timeout),
                write_timeout=1.0,
            )
        except serial.SerialException as exc:
            message = str(exc)
            message_lower = message.lower()
            if sys.platform.startswith("win") and (
                "access is denied" in message_lower
                or "permissionerror" in message_lower
                or "拒绝访问" in message
            ):
                message += (
                    "\nWindows 拒绝访问该 COM 口。请关闭串口助手、Modbus 工具、"
                    "触摸屏下载工具等可能占用串口的软件，然后重新插拔转换器再试。"
                )
            elif "permission denied" in message_lower:
                message += (
                    "\n当前用户没有串口权限。可临时用 sudo 运行；长期方案是将用户加入"
                    " dialout 组后重新登录。"
                )
            elif sys.platform.startswith("win"):
                message += (
                    "\n请在设备管理器的“端口 (COM 和 LPT)”中确认 COM 号和"
                    "CH340/CH341 驱动状态，并用 --port COMx 指定正确端口。"
                )
            raise ModbusError(f"无法打开串口 {self.port_name}: {message}") from exc
        return self

    def __exit__(self, *_: object) -> None:
        if self.port is not None:
            self.port.close()
            self.port = None

    def _collect_response(self, request: bytes) -> bytes:
        assert self.port is not None
        received = bytearray()
        deadline = time.monotonic() + self.timeout
        last_received_at: float | None = None

        # 收到数据后连续空闲 30 ms，认为一帧结束。19200 波特率下远大于
        # Modbus RTU 要求的 3.5 字符间隔，同时不会让扫描等待太久。
        while time.monotonic() < deadline:
            chunk = self.port.read(256)
            if chunk:
                received.extend(chunk)
                last_received_at = time.monotonic()
            elif last_received_at is not None and time.monotonic() - last_received_at >= 0.03:
                break

        raw = bytes(received)
        # 少数转换器会把发送帧原样回显，自动剥掉完整回显。
        if len(raw) > len(request) and raw.startswith(request):
            raw = raw[len(request) :]
        if not raw:
            raise ModbusTimeout("等待响应超时")
        return raw

    def _request(self, slave: int, function: int, payload: bytes) -> bytes:
        if not 1 <= slave <= 247:
            raise ValueError("Modbus 从站地址必须为 1～247")
        assert self.port is not None

        request = add_crc(bytes((slave, function)) + payload)
        self.port.reset_input_buffer()
        if self.verbose:
            print(f"TX: {hex_frame(request)}")
        self.port.write(request)
        self.port.flush()
        response = self._collect_response(request)
        if self.verbose:
            print(f"RX: {hex_frame(response)}")

        if len(response) < 5:
            raise ModbusProtocolError(f"响应过短：{hex_frame(response)}")
        expected_crc = int.from_bytes(response[-2:], byteorder="little")
        actual_crc = crc16_modbus(response[:-2])
        if expected_crc != actual_crc:
            raise ModbusProtocolError(
                f"CRC 错误：收到 0x{expected_crc:04X}，计算得到 0x{actual_crc:04X}；"
                f"原始帧 {hex_frame(response)}"
            )
        if response[0] != slave:
            raise ModbusProtocolError(
                f"从站地址不匹配：请求 {slave}，响应 {response[0]}"
            )
        if response[1] == (function | 0x80):
            raise ModbusExceptionResponse(response[2])
        if response[1] != function:
            raise ModbusProtocolError(
                f"功能码不匹配：请求 0x{function:02X}，响应 0x{response[1]:02X}"
            )
        return response

    def read_holding_registers(self, slave: int, start: int, count: int) -> list[int]:
        if not 0 <= start <= 0xFFFF:
            raise ValueError("起始寄存器必须为 0x0000～0xFFFF")
        if not 1 <= count <= 125 or start + count > 0x10000:
            raise ValueError("读取数量必须为 1～125，且不能超过 0xFFFF")
        payload = start.to_bytes(2, "big") + count.to_bytes(2, "big")
        response = self._request(slave, 0x03, payload)
        byte_count = response[2]
        expected_length = 5 + byte_count
        if len(response) != expected_length:
            raise ModbusProtocolError(
                f"0x03 响应长度错误：应为 {expected_length}，实际 {len(response)}"
            )
        if byte_count != count * 2:
            raise ModbusProtocolError(
                f"0x03 数据长度错误：请求 {count * 2} 字节，响应 {byte_count} 字节"
            )
        return [
            int.from_bytes(response[index : index + 2], "big")
            for index in range(3, 3 + byte_count, 2)
        ]

    def write_multiple_registers(
        self, slave: int, start: int, values: Sequence[int]
    ) -> None:
        if not 0 <= start <= 0xFFFF:
            raise ValueError("起始寄存器必须为 0x0000～0xFFFF")
        if not 1 <= len(values) <= 123 or start + len(values) > 0x10000:
            raise ValueError("写入数量必须为 1～123，且不能超过 0xFFFF")
        if any(not 0 <= value <= 0xFFFF for value in values):
            raise ValueError("寄存器值必须为 0～65535")

        data = b"".join(value.to_bytes(2, "big") for value in values)
        payload = (
            start.to_bytes(2, "big")
            + len(values).to_bytes(2, "big")
            + bytes((len(data),))
            + data
        )
        response = self._request(slave, 0x10, payload)
        if len(response) != 8:
            raise ModbusProtocolError(f"0x10 响应长度应为 8，实际为 {len(response)}")
        echoed_start = int.from_bytes(response[2:4], "big")
        echoed_count = int.from_bytes(response[4:6], "big")
        if echoed_start != start or echoed_count != len(values):
            raise ModbusProtocolError(
                "0x10 响应中的起始地址或写入数量与请求不一致"
            )


@dataclass(frozen=True)
class RegisterInfo:
    name: str
    validator: Callable[[int], bool] | None = None
    expected: str = ""


def one_of(*values: int) -> Callable[[int], bool]:
    allowed = set(values)
    return lambda value: value in allowed


REGISTER_INFO: dict[int, RegisterInfo] = {
    0x0001: RegisterInfo("手动/自动", one_of(0, 1), "0=手动，1=自动"),
    0x0002: RegisterInfo("加热输出", one_of(0, 1), "0=关，1=开"),
    0x0003: RegisterInfo("风扇档位", one_of(0, 1, 2, 3), "0～3"),
    0x0004: RegisterInfo("三色灯", one_of(0, 1, 2, 3), "0～3"),
    0x0005: RegisterInfo("蜂鸣器", one_of(0, 1), "0=关，1=开"),
    0x0021: RegisterInfo("当前加热输出", one_of(0, 1), "0=关，1=开"),
    0x0022: RegisterInfo("当前风扇档位", one_of(0, 1, 2, 3), "0～3"),
    0x0023: RegisterInfo("当前报警代码"),
    0x0024: RegisterInfo("当前高压状态", one_of(0, 1), "0=断电，1=通电"),
    0x0025: RegisterInfo("当前子板运行状态", one_of(0, 10, 11), "0/10/11"),
    0x0061: RegisterInfo("PT100 1 温度（倍率待确认）"),
    0x0062: RegisterInfo("PT100 2 温度（倍率待确认）"),
    0x0063: RegisterInfo("PT100 平均温度（倍率待确认）"),
    0x0081: RegisterInfo("自动加热温度下限"),
    0x0082: RegisterInfo("自动加热温度上限"),
    0x0083: RegisterInfo("风扇1档湿度上限"),
    0x0084: RegisterInfo("风扇1档湿度下限"),
    0x0085: RegisterInfo("风扇2档湿度上限"),
    0x0086: RegisterInfo("风扇2档湿度下限"),
    0x0087: RegisterInfo("风扇3档湿度上限"),
    0x0088: RegisterInfo("风扇3档湿度下限"),
    0x0091: RegisterInfo("温湿度传感器总数"),
    0x0092: RegisterInfo("温湿度轮询间隔/ms"),
    0x0093: RegisterInfo("PT100 传感器总数"),
    0x0094: RegisterInfo("PT100 轮询间隔/ms"),
    0x00A1: RegisterInfo("旧文档：温湿度传感器总数"),
    0x00A2: RegisterInfo("旧文档：PT100 传感器总数"),
    0x00E1: RegisterInfo("自动启动/停止", one_of(0, 1), "0=停止，1=启动"),
    0x00EE: RegisterInfo("参数保存状态", one_of(0, 1, 2), "0/1/2"),
}

for sensor_number, start in enumerate(range(0x0041, 0x004D, 2), start=1):
    REGISTER_INFO[start] = RegisterInfo(f"温湿度传感器{sensor_number} 温度（倍率待确认）")
    REGISTER_INFO[start + 1] = RegisterInfo(f"温湿度传感器{sensor_number} 湿度（倍率待确认）")
REGISTER_INFO[0x004D] = RegisterInfo("温湿度平均温度（倍率待确认）")
REGISTER_INFO[0x004E] = RegisterInfo("温湿度平均湿度（倍率待确认）")


SNAPSHOT_GROUPS = (
    ("控制寄存器", 0x0001, 5),
    ("状态寄存器", 0x0021, 5),
    ("温湿度", 0x0041, 14),
    ("PT100", 0x0061, 3),
    ("自动阈值", 0x0081, 8),
    ("总线配置（新表）", 0x0091, 4),
    ("总线配置（旧表）", 0x00A1, 2),
    ("自动启停", 0x00E1, 1),
    ("参数保存", 0x00EE, 1),
)


READ_ONLY_RANGES = (
    range(0x0021, 0x0026),
    range(0x0041, 0x004F),
    range(0x0061, 0x0064),
)


def is_documented_read_only(address: int) -> bool:
    return any(address in address_range for address_range in READ_ONLY_RANGES)


def print_registers(start: int, values: Sequence[int]) -> None:
    print("地址      十进制  十六进制  名称/检查")
    for offset, value in enumerate(values):
        address = start + offset
        info = REGISTER_INFO.get(address)
        description = info.name if info else "未在文档中定义"
        if info and info.validator and not info.validator(value):
            description += f"  [异常：预期 {info.expected}]"
        print(f"0x{address:04X}  {value:7d}  0x{value:04X}    {description}")


def looks_like_address_plus_ten(samples: Iterable[tuple[int, int]]) -> bool:
    pairs = list(samples)
    return len(pairs) >= 4 and all(value == address + 10 for address, value in pairs)


def make_master(args: argparse.Namespace) -> ModbusRTUMaster:
    if not args.port:
        raise ModbusError("内部错误：串口尚未解析")
    return ModbusRTUMaster(
        port=args.port,
        baudrate=args.baud,
        timeout=args.timeout,
        verbose=args.verbose,
    )


def command_ports(_: argparse.Namespace) -> int:
    ports = list(list_ports.comports())
    if not ports:
        print("没有发现串口设备。")
        if sys.platform.startswith("win"):
            print("请检查设备管理器中的 CH340/CH341 驱动和“端口 (COM 和 LPT)”。")
        return 1
    automatic_port: str | None = None
    try:
        automatic_port = choose_automatic_port(ports, sys.platform)
    except ModbusError:
        # ports 的职责是完整列出设备；自动选择有歧义时不应阻止列表输出。
        pass
    for item in ports:
        marker = "  [自动选择]" if item.device == automatic_port else ""
        print(f"{describe_port(item)}{marker}")
    if automatic_port is None:
        platform_example = "COM3" if sys.platform.startswith("win") else "/dev/ttyUSB0"
        print(f"未自动选择端口；运行其他命令时请添加 --port {platform_example}。")
    return 0


def command_scan(args: argparse.Namespace) -> int:
    found: list[int] = []
    with make_master(args) as master:
        for slave in range(args.start_id, args.end_id + 1):
            try:
                values = master.read_holding_registers(slave, args.register, 1)
            except ModbusTimeout:
                print(f"从站 {slave:3d}: 无响应")
            except ModbusError as exc:
                print(f"从站 {slave:3d}: 收到响应，但有错误：{exc}")
                found.append(slave)
            else:
                print(
                    f"从站 {slave:3d}: 正常，0x{args.register:04X} = {values[0]} "
                    f"(0x{values[0]:04X})"
                )
                found.append(slave)
    print("扫描结果：" + (", ".join(map(str, found)) if found else "未找到从站"))
    return 0 if found else 2


def command_read(args: argparse.Namespace) -> int:
    with make_master(args) as master:
        values = master.read_holding_registers(args.slave, args.start, args.count)
    print_registers(args.start, values)
    if looks_like_address_plus_ten(
        (args.start + offset, value) for offset, value in enumerate(values)
    ):
        print("\n警告：本次数据全部符合“寄存器地址 + 10”，疑似 Modbus 测试固件。")
    return 0


def command_snapshot(args: argparse.Namespace) -> int:
    all_samples: list[tuple[int, int]] = []
    failed = 0
    with make_master(args) as master:
        for group_name, start, count in SNAPSHOT_GROUPS:
            print(f"\n[{group_name}] 0x{start:04X}，数量 {count}")
            try:
                values = master.read_holding_registers(args.slave, start, count)
            except ModbusError as exc:
                failed += 1
                print(f"读取失败：{exc}")
                continue
            print_registers(start, values)
            all_samples.extend(
                (start + offset, value) for offset, value in enumerate(values)
            )

    if looks_like_address_plus_ten(all_samples):
        print(
            "\n*** 诊断结论：所有成功读取的数据均为“寄存器地址 + 10”，"
            "机器很可能运行 Modbus 示例/测试固件。 ***"
        )
    elif all_samples:
        print("\n快照完成：未发现统一的“地址 + 10”测试数据规律。")
    return 1 if failed else 0


def command_monitor(args: argparse.Namespace) -> int:
    iteration = 0
    try:
        with make_master(args) as master:
            while args.iterations == 0 or iteration < args.iterations:
                iteration += 1
                timestamp = datetime.now().isoformat(timespec="milliseconds")
                try:
                    values = master.read_holding_registers(
                        args.slave, args.start, args.count
                    )
                    rendered = ", ".join(
                        f"0x{args.start + offset:04X}={value}"
                        for offset, value in enumerate(values)
                    )
                    print(f"{timestamp}  {rendered}", flush=True)
                except ModbusError as exc:
                    print(f"{timestamp}  ERROR: {exc}", flush=True)
                if args.iterations == 0 or iteration < args.iterations:
                    time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n监视已停止。")
    return 0


def validate_write_request(start: int, values: Sequence[int], force: bool) -> None:
    for offset, value in enumerate(values):
        address = start + offset
        if is_documented_read_only(address) and not force:
            raise ModbusError(
                f"0x{address:04X} 在文档中是只读寄存器；若已向固件工程师确认，"
                "可加 --force 覆盖保护。"
            )
        info = REGISTER_INFO.get(address)
        if info and info.validator and not info.validator(value) and not force:
            raise ModbusError(
                f"0x{address:04X}={value} 超出文档范围（{info.expected}）；"
                "若确实需要，可加 --force。"
            )


def command_write(args: argparse.Namespace) -> int:
    if not args.allow_write:
        raise ModbusError(
            "写操作被安全锁阻止。确认现场安全并获得负责人许可后，加 --allow-write。"
        )
    validate_write_request(args.start, args.values, args.force)
    end = args.start + len(args.values) - 1
    print("危险：即将使用功能码 0x10 写机器寄存器，可能使继电器或设备动作。")
    print(
        f"串口={args.port}，从站={args.slave}，地址=0x{args.start:04X}"
        f"～0x{end:04X}，值={args.values}"
    )
    confirmation = input("确认现场安全后，输入大写 WRITE 继续：")
    if confirmation != "WRITE":
        print("已取消，没有发送写请求。")
        return 2

    with make_master(args) as master:
        master.write_multiple_registers(args.slave, args.start, args.values)
        readback = master.read_holding_registers(args.slave, args.start, len(args.values))
    print(f"写请求已被从站确认；读回值：{readback}")
    if list(readback) != list(args.values):
        print("警告：读回值与写入值不一致。")
        return 1
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="RS485 / Modbus RTU 调试工具（默认 19200 8N1、只读）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "地址可写成十进制或 0x 开头的十六进制，例如 33 和 0x0021 等价。\n"
            "板卡图片标注：上位机使用隔离 RS485 的 B1-/A1+ 接口。"
        ),
    )
    parser.add_argument(
        "--port",
        default=None,
        help="串口；默认自动识别 CH340/CH341（例如 Linux /dev/ttyUSB0、Windows COM3）",
    )
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="波特率（默认 19200）")
    parser.add_argument("--timeout", type=float, default=0.35, help="响应超时秒数（默认 0.35）")
    parser.add_argument("-v", "--verbose", action="store_true", help="显示原始 TX/RX 帧")

    subparsers = parser.add_subparsers(dest="command", required=True)

    ports_parser = subparsers.add_parser("ports", help="列出串口设备")
    ports_parser.set_defaults(handler=command_ports)

    scan_parser = subparsers.add_parser("scan", help="扫描 Modbus 从站（只读）")
    scan_parser.add_argument("--start-id", type=int, default=1, help="起始从站地址（默认 1）")
    scan_parser.add_argument("--end-id", type=int, default=15, help="结束从站地址（默认 15）")
    scan_parser.add_argument(
        "--register", type=parse_number, default=0x0021, help="探测寄存器（默认 0x0021）"
    )
    scan_parser.set_defaults(handler=command_scan)

    read_parser = subparsers.add_parser("read", help="读取任意保持寄存器（0x03）")
    read_parser.add_argument("start", type=parse_number, help="起始寄存器")
    read_parser.add_argument("count", type=int, help="寄存器数量 1～125")
    read_parser.add_argument("--slave", type=int, default=DEFAULT_SLAVE, help="从站地址（默认 1）")
    read_parser.set_defaults(handler=command_read)

    snapshot_parser = subparsers.add_parser("snapshot", help="读取文档中的全部寄存器组")
    snapshot_parser.add_argument("--slave", type=int, default=DEFAULT_SLAVE, help="从站地址（默认 1）")
    snapshot_parser.set_defaults(handler=command_snapshot)

    monitor_parser = subparsers.add_parser("monitor", help="周期监视一组寄存器，Ctrl+C 停止")
    monitor_parser.add_argument("start", type=parse_number, help="起始寄存器")
    monitor_parser.add_argument("count", type=int, help="寄存器数量 1～125")
    monitor_parser.add_argument("--slave", type=int, default=DEFAULT_SLAVE, help="从站地址（默认 1）")
    monitor_parser.add_argument("--interval", type=float, default=1.0, help="轮询间隔秒数（默认 1）")
    monitor_parser.add_argument(
        "--iterations", type=int, default=0, help="读取次数；0 表示一直运行（默认 0）"
    )
    monitor_parser.set_defaults(handler=command_monitor)

    write_parser = subparsers.add_parser("write", help="写保持寄存器（0x10，有安全确认）")
    write_parser.add_argument("start", type=parse_number, help="起始寄存器")
    write_parser.add_argument("values", type=parse_number, nargs="+", help="一个或多个写入值")
    write_parser.add_argument("--slave", type=int, default=DEFAULT_SLAVE, help="从站地址（默认 1）")
    write_parser.add_argument(
        "--allow-write", action="store_true", help="确认已获许可并解除第一层写保护"
    )
    write_parser.add_argument(
        "--force", action="store_true", help="允许写只读地址或超出文档范围的值（非常谨慎）"
    )
    write_parser.set_defaults(handler=command_write)

    return parser


def validate_common_args(args: argparse.Namespace) -> None:
    if args.baud <= 0:
        raise ModbusError("波特率必须大于 0")
    if args.timeout <= 0:
        raise ModbusError("超时时间必须大于 0")
    if args.command == "scan":
        if not 1 <= args.start_id <= args.end_id <= 247:
            raise ModbusError("扫描地址必须满足 1 <= start-id <= end-id <= 247")
    if args.command == "monitor":
        if args.interval < 0.05:
            raise ModbusError("为避免占满总线，监视间隔不能小于 0.05 秒")
        if args.iterations < 0:
            raise ModbusError("读取次数不能为负数")
    if args.command != "ports":
        requested_port = args.port
        args.port = resolve_serial_port(requested_port)
        if requested_port is None:
            print(f"自动选择串口：{args.port}")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        validate_common_args(args)
        return int(args.handler(args))
    except (ModbusError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
