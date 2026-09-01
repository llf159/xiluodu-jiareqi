#ifndef RS485DEVICE_H
#define RS485DEVICE_H

// ============================================================
// RS485 与设备交互层
// 包含: DeviceProfile (设备类型与寄存器映射)
//       ModbusFrame   (RTU 帧构建与解析)
//       ModbusRtu     (高层读写接口)
//       SerialPortWorker (串口工作线程对象)
// ============================================================

#include <QString>
#include <QMap>
#include <QVariant>
#include <QDateTime>
#include <QByteArray>
#include <QVector>
#include <QObject>
#include <QTimer>
#include <QSerialPort>
#include <QQueue>
#include <QMutex>
#include <functional>

/**
 * @brief 下位机 PCB 板设备模型
 *
 * 协议: Modbus 地址表 [26.08.17]
 * 通信: Modbus RTU, 19200/N/8/1, 读 0x03 / 写 0x10
 * 所有设备为同型 PCB 板, 板上四位拨码开关设置从站地址
 * 运行时数据以 key-value 形式存储，便于 UI 展示和 CSV 记录
 */
class DeviceProfile
{
public:
    enum DeviceType {
        PcbBoard = 0            // 下位机 PCB 板 (唯一设备类型)
    };

    /** 寄存器地址 (Modbus 报文中直接使用的原始地址) */
    enum RegAddr {
        RegHvInput  = 0x0001, // 只读: 高压通电输入 0-无 1-有
        RegReserved = 0x0002, // 只读: 备用
        RegVoltage  = 0x0003, // 只读: 外部电压, 原始值放大 10 倍
        RegThBase   = 0x0004, // 只读: 温湿度传感器1~3, 温度在前湿度在后, 放大10倍
        RegPtBase   = 0x000A, // 只读: PT100 1~2 温度, 放大10倍
        RegOtBase   = 0x0011  // 读写: OT01~OT10, 0-关 1-打开
    };

    /** 设备唯一标识: portIndex + slaveId */
    struct DeviceKey {
        int portIndex = 0;
        int slaveId = 0;

        bool operator==(const DeviceKey &o) const {
            return portIndex == o.portIndex && slaveId == o.slaveId;
        }
    };

    /** 单段连续读区 */
    struct ReadSegment {
        quint16 startAddr = 0;
        quint16 count = 0;
    };

    /** 轮询读取映射: 协议寄存器分散在多个区, 需多段读取 */
    struct RegisterMap {
        QVector<ReadSegment> readSegments;
    };

    /** 一次 0x10 写请求 (起始地址 + 连续值) */
    struct WriteItem {
        quint16 startAddr = 0;
        QVector<quint16> values;
    };

    static QString deviceTypeToString(DeviceType t);
    static DeviceType deviceTypeFromString(const QString &s);
    static QString deviceTypeDisplayName(DeviceType t);

    /** PCB 板轮询读取的寄存器分段 */
    static RegisterMap defaultRegisterMap(DeviceType t);

    /** 解析单段寄存器读值并合并到 out (温度按 INT16 原始值解析) */
    static void parseSegment(quint16 startAddr, const QVector<quint16> &regs,
                             QMap<QString, QVariant> &out);

    /** 将 UI 字段编码为 0x10 写请求列表 (相邻地址自动合并) */
    static QVector<WriteItem> encodeWriteValues(DeviceType t,
                                                const QMap<QString, QVariant> &fields);

    /** UI 展示辅助: 字段显示顺序 / 中文名 / 值格式化 */
    static QStringList orderedFields();
    static QString fieldDisplayName(const QString &field);
    static QString fieldDisplayValue(const QString &field, const QVariant &value);
};

/** 供 QHash 使用 */
inline uint qHash(const DeviceProfile::DeviceKey &k, uint seed = 0)
{
    return qHash(k.portIndex, seed) ^ qHash(k.slaveId, seed << 1);
}

Q_DECLARE_METATYPE(DeviceProfile::DeviceKey)

/**
 * @brief Modbus RTU 帧构建与解析 (标准功能码)
 *
 * 支持:
 *   0x03 读保持寄存器
 *   0x04 读输入寄存器
 *   0x06 写单个寄存器
 *   0x10 写多个寄存器
 */
namespace ModbusFrame {

enum FunctionCode : quint8 {
    ReadHoldingRegisters   = 0x03,
    ReadInputRegisters     = 0x04,
    WriteSingleRegister    = 0x06,
    WriteMultipleRegisters = 0x10
};

/** CRC16 Modbus (多项式 0xA001) */
quint16 crc16(const QByteArray &data);

/** 构建 RTU 请求帧 (不含前后静默时间) */
QByteArray buildReadRequest(quint8 slaveId, FunctionCode fc,
                            quint16 startAddr, quint16 count);
QByteArray buildWriteSingleRequest(quint8 slaveId, quint16 addr, quint16 value);
QByteArray buildWriteMultipleRequest(quint8 slaveId, quint16 startAddr,
                                     const QVector<quint16> &values);

/**
 * @brief 解析 RTU 响应
 * @return true 表示 CRC 正确且 slaveId/fc 匹配
 */
bool parseReadResponse(const QByteArray &frame, quint8 expectedSlave,
                       quint8 expectedFc, QVector<quint16> &outValues);

bool parseWriteResponse(const QByteArray &frame, quint8 expectedSlave,
                        quint8 expectedFc);

/** 从串口缓冲区提取完整 RTU 帧 (根据长度字段判断) */
bool extractFrame(QByteArray &buffer, QByteArray &outFrame);

} // namespace ModbusFrame

/**
 * @brief Modbus RTU 高层接口 (在 SerialPortWorker 线程中调用)
 *
 * 封装请求-响应流程，供 PollScheduler 使用
 */
class ModbusRtu
{
public:
    struct Result {
        bool success = false;
        QVector<quint16> registers;
        QString error;
    };

    /**
     * @param sendAndReceive 由 SerialPortWorker 提供的底层收发函数
     * @param timeoutMs      超时毫秒
     */
    using TransactFunc = std::function<Result(const QByteArray &request,
                                              quint8 expectedFc)>;

    static Result readHoldingRegisters(TransactFunc transact,
                                       quint8 slaveId,
                                       quint16 startAddr,
                                       quint16 count);

    static Result writeSingleRegister(TransactFunc transact,
                                      quint8 slaveId,
                                      quint16 addr,
                                      quint16 value);

    static Result writeMultipleRegisters(TransactFunc transact,
                                         quint8 slaveId,
                                         quint16 startAddr,
                                         const QVector<quint16> &values);
};

/**
 * @brief 单个物理 RS485 口的通信工作线程对象
 *
 * 设计要点:
 * - 每个物理口独占一个 QThread + SerialPortWorker，实现多口并发
 * - 同一条总线上的多个从站在本 worker 内串行访问 (Modbus 半双工要求)
 * - 使用任务队列处理读写请求，轮询与写操作统一调度，避免冲突
 * - 写操作优先于尚未开始的轮询，避免离线从站超时阻塞用户控制
 * - QTimer 驱动轮询，不使用 busy-loop，降低 CPU 占用
 */
class SerialPortWorker : public QObject
{
    Q_OBJECT
public:
    struct PortSettings {
        int portIndex = 0;
        QString portName;
        QString device;
        int baudRate = 19200; // 协议固定 19200/N/8/1
        int dataBits = 8;
        QSerialPort::Parity parity = QSerialPort::NoParity;
        QSerialPort::StopBits stopBits = QSerialPort::OneStop;
        int frameDelayMs = 5;
        int timeoutMs = 500;
        int interSlaveDelayMs = 50;
    };

    struct PollTask {
        DeviceProfile::DeviceKey deviceKey;
        DeviceProfile::DeviceType deviceType;
        DeviceProfile::RegisterMap regMap;
    };

    struct WriteTask {
        DeviceProfile::DeviceKey deviceKey;
        DeviceProfile::DeviceType deviceType;
        QMap<QString, QVariant> fields;   // 由 DeviceProfile::encodeWriteValues 编码
    };

    explicit SerialPortWorker(const PortSettings &settings, QObject *parent = nullptr);
    ~SerialPortWorker() override;

public slots:
    void startWork();
    void stopWork();
    void startPolling(int intervalMs);
    void stopPolling();
    void setPollTasks(const QVector<PollTask> &tasks);
    void restartDiscovery();
    void enqueueWrite(const WriteTask &task);

signals:
    void deviceDataReady(DeviceProfile::DeviceKey key,
                         const QMap<QString, QVariant> &values,
                         bool online);
    void writeFinished(DeviceProfile::DeviceKey key, bool success, const QString &error);
    void portError(int portIndex, const QString &error);

private slots:
    void onPollTimer();
    void processQueue();

private:
    ModbusRtu::Result transact(const QByteArray &request, quint8 expectedFc,
                               int timeoutMs = -1);
    bool openPort();
    void closePort();

    PortSettings m_settings;
    QSerialPort *m_serial = nullptr;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_queueTimer = nullptr;

    QVector<PollTask> m_pollTasks;

    enum QueueItemType { Poll, Discover, Write };
    struct QueueItem {
        QueueItemType type;
        PollTask poll;
        WriteTask write;
        int discoverySlaveId = 0;
    };
    QQueue<QueueItem> m_queue;
    QMutex m_queueMutex;
    bool m_busy = false;
    int m_nextDiscoverySlaveId = 1;
    QByteArray m_rxBuffer;
};

Q_DECLARE_METATYPE(SerialPortWorker::PollTask)
Q_DECLARE_METATYPE(SerialPortWorker::WriteTask)
Q_DECLARE_METATYPE(QVector<SerialPortWorker::PollTask>)

#endif // RS485DEVICE_H
