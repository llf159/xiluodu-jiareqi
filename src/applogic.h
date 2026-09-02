#ifndef APPLOGIC_H
#define APPLOGIC_H

// ============================================================
// 业务逻辑控制层
// 包含: AppConfig       (app.ini 配置加载/保存)
//       DeviceManager   (逻辑设备状态管理)
//       PollScheduler   (轮询调度, 每口一线程)
//       DataLogger      (CSV 数据记录)
//       HistoryQuery    (历史数据查询)
//       StorageRotator  (存储清理轮转)
// ============================================================

#include <QString>
#include <QVector>
#include <QObject>
#include <QHash>
#include <QThread>
#include <QMap>
#include <QVariant>
#include <QMutex>
#include <QDate>
#include <QTextStream>
#include <QTimer>
#include "rs485device.h"

/**
 * @brief 应用全局配置，从 app.ini 加载/保存
 *
 * 配置项包括: 存储路径、轮询参数和两个物理串口；子板由运行时扫描发现
 */
class AppConfig
{
public:
    struct PortConfig {
        bool enabled = false;
        QString name;
        QString device;       // COM3 或 /dev/ttyS1
        int baudRate = 19200;  // 协议固定 19200/N/8/1
        int dataBits = 8;
        char parity = 'N';    // N/E/O
        int stopBits = 1;
        int frameDelayMs = 5;
    };

    struct GeneralConfig {
        QString dataPath;
        int maxStorageMB = 12288;
        int pollIntervalMs = 1000;
        int modbusTimeoutMs = 500;
        int interSlaveDelayMs = 50;
        double temperatureTarget = 25.0;
        QString temperatureTargetSource = "pt100";
        QString temperatureControlMode = "threshold";
        double thresholdSingleStageDelta = 0.3;
        double thresholdSecondStageDelta = 1.5;
        double thresholdDualStageDelta = 3.0;
        double thresholdHysteresis = 0.2;
        double dewPointSingleStageMargin = 3.0;
        double dewPointSecondStageMargin = 2.0;
        double dewPointDualStageMargin = 1.0;
        double dewPointHysteresis = 0.2;
        double humidityTemperatureLimitDelta = 5.0;
        double pidKp = 12.0;
        double pidKi = 0.15;
        double pidKd = 0.0;
        double pidSingleStagePercent = 10.0;
        double pidSecondStagePercent = 35.0;
        double pidDualStagePercent = 60.0;
        QString pidFirstStageOutput = "ot3";
        QString spareOt01Mode = "off";
        QString spareOt02Mode = "off";
        QString spareOt05Mode = "off";
        QString spareOt06Mode = "off";
        QString reservedInputMode = "monitor";
        QString highVoltageDetectionMode = "analog";
        int highVoltageDigitalTrigger = 1;
        double highVoltageThreshold = 1.0;
        int relaySwitchIntervalSec = 10;
        int recordIntervalSec = 1;
    };

    static AppConfig &instance();

    bool load(const QString &iniPath);
    bool save(const QString &iniPath) const;

    const GeneralConfig &general() const { return m_general; }
    GeneralConfig &general() { return m_general; }

    const QVector<PortConfig> &ports() const { return m_ports; }
    QVector<PortConfig> &ports() { return m_ports; }

    const QStringList &validationErrors() const { return m_validationErrors; }

    QString configFilePath() const { return m_configPath; }

private:
    AppConfig() = default;

    GeneralConfig m_general;
    QVector<PortConfig> m_ports;
    QStringList m_validationErrors;
    QString m_configPath;
};

/**
 * @brief 单个逻辑设备的运行时状态
 */
struct DeviceState {
    DeviceProfile::DeviceKey key;
    QString name;
    DeviceProfile::DeviceType type = DeviceProfile::PcbBoard;
    bool online = false;
    bool selected = false;          // UI 是否选中参与轮询
    QDateTime lastUpdate;
    QMap<QString, QVariant> values; // 如 hv_input, external_voltage, th1_temp, ot01 等协议字段
    QString lastError;
};

/**
 * @brief 管理两路 RS485、每路最多 16 个逻辑设备的状态
 */
class DeviceManager : public QObject
{
    Q_OBJECT
public:
    explicit DeviceManager(QObject *parent = nullptr);

    void clearDiscoveredDevices();
    QList<DeviceState> allDevices() const;
    DeviceState device(const DeviceProfile::DeviceKey &key) const;
    bool hasDevice(const DeviceProfile::DeviceKey &key) const;

    void setSelected(const DeviceProfile::DeviceKey &key, bool selected);
    void setSelectedAll(bool selected);
    QList<DeviceState> selectedDevices() const;

public slots:
    void updateDeviceData(const DeviceProfile::DeviceKey &key,
                          const QMap<QString, QVariant> &values,
                          bool online);

signals:
    void deviceUpdated(const DeviceProfile::DeviceKey &key);
    void devicesChanged();

private:
    QHash<DeviceProfile::DeviceKey, DeviceState> m_devices;
};

/**
 * @brief 数据存储: 按日期分文件的 CSV
 *
 * 文件格式: logs/YYYY-MM-DD.csv
 * 新文件只保留设备身份、必要测量值、备用输入和 OT1～OT6 状态。
 * 不使用数据库，保持 RK3568 上的低内存占用。
 */
class DataLogger : public QObject
{
    Q_OBJECT
public:
    explicit DataLogger(QObject *parent = nullptr);

    void setDataPath(const QString &path);
    QString dataPath() const { return m_dataPath; }

    void appendRecord(const DeviceProfile::DeviceKey &key,
                      const QString &deviceName,
                      DeviceProfile::DeviceType type,
                      const QMap<QString, QVariant> &values);

signals:
    void logError(const QString &message);
    void recordAppended(const QDateTime &timestamp,
                        const DeviceProfile::DeviceKey &key,
                        const QString &deviceName,
                        DeviceProfile::DeviceType type,
                        const QMap<QString, QVariant> &values);

private:
    QString csvFilePathForDate(const QDate &date) const;
    void ensureHeader(QTextStream &out, const QString &filePath);

    QString m_dataPath;
    QMutex m_mutex;
};

/**
 * @brief 历史数据查询 (读取 CSV 文件)
 */
class HistoryQuery : public QObject
{
    Q_OBJECT
public:
    struct DeviceInfo {
        int portIndex = 0;
        int slaveId = 0;
        QString name;
    };

    struct Record {
        QDateTime timestamp;
        int portIndex = 0;
        int slaveId = 0;
        QString deviceName;
        DeviceProfile::DeviceType deviceType = DeviceProfile::PcbBoard;
        QMap<QString, QVariant> values;
    };

    struct Filter {
        QDate dateFrom;
        QDate dateTo;
        int portIndex = -1;     // -1 表示不限
        int slaveId = -1;       // -1 表示不限
        QString deviceType;     // 空表示不限
    };

    explicit HistoryQuery(QObject *parent = nullptr);

    void setDataPath(const QString &path);

    /** 同步查询，数据量受 30 天范围限制，适合触摸屏本地使用 */
    QVector<Record> query(const Filter &filter) const;
    QVector<DeviceInfo> availableDevices() const;

private:
    QVector<Record> parseFile(const QString &filePath, const Filter &filter) const;

    QString m_dataPath;
};

/**
 * @brief 存储轮转: 超过日志容量上限时清理最旧 CSV
 *
 * 定时检查 (默认每小时)，删除超期文件；
 * 若总大小超过 maxStorageMB，继续删除最旧文件直至达标
 */
class StorageRotator : public QObject
{
    Q_OBJECT
public:
    struct StorageStatus {
        bool ready = false;
        qint64 bytesTotal = 0;
        qint64 bytesAvailable = 0;
        qint64 logBytes = 0;
        int fileCount = 0;
        QDate oldestDate;
        QDate newestDate;
        QString error;
    };

    struct DeleteResult {
        bool valid = false;
        int files = 0;
        qint64 bytes = 0;
        QString error;
    };

    explicit StorageRotator(QObject *parent = nullptr);

    void setDataPath(const QString &path);
    void setMaxStorageMB(int mb);

    void start(int checkIntervalMs = 3600000);
    void stop();
    StorageStatus storageStatus() const;
    DeleteResult previewDeleteBefore(const QDate &cutoff) const;
    DeleteResult deleteBefore(const QDate &cutoff);

public slots:
    void runCleanup();

signals:
    void cleanupDone(int filesRemoved, qint64 bytesFreed);

private:
    QString m_dataPath;
    int m_maxStorageMB = 12288;
    QTimer *m_timer = nullptr;
};

/**
 * @brief 轮询调度器
 *
 * - 每个启用的物理口创建一个独立 QThread + SerialPortWorker (并发)
 * - 同口内多从站由 Worker 队列串行，口与口之间并行
 * - 自动扫描每口 Modbus ID 1~247，每口最多发现 16 块子板
 * - 仅轮询 DeviceManager 中 selected 的已发现设备
 * - 收到数据后更新 DeviceManager 并写入 DataLogger
 */
class PollScheduler : public QObject
{
    Q_OBJECT
public:
    explicit PollScheduler(DeviceManager *deviceMgr,
                           DataLogger *logger,
                           QObject *parent = nullptr);
    ~PollScheduler() override;

    bool start();
    void stop();
    void refreshPollTasks();
    void rescanDevices();

public slots:
    void writeToDevice(const DeviceProfile::DeviceKey &key,
                       const QMap<QString, QVariant> &fields);

signals:
    void writeCompleted(const DeviceProfile::DeviceKey &key,
                        bool success,
                        const QString &error);
    void schedulerError(const QString &message);

private slots:
    void onDeviceDataReady(DeviceProfile::DeviceKey key,
                           const QMap<QString, QVariant> &values,
                           bool online);
    void onWriteFinished(DeviceProfile::DeviceKey key, bool success, const QString &error);

private:
    struct PortHandle {
        QThread *thread = nullptr;
        SerialPortWorker *worker = nullptr;
    };

    void setupPortWorker(int portIndex, const AppConfig::PortConfig &portCfg);

    DeviceManager *m_deviceMgr = nullptr;
    DataLogger *m_logger = nullptr;
    QHash<int, PortHandle> m_ports;
    QHash<DeviceProfile::DeviceKey, QDateTime> m_lastLogTimes;
};

#endif // APPLOGIC_H
