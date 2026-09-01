// ============================================================
// 业务逻辑控制层实现
// 合并自: appconfig.cpp / devicemanager.cpp / pollscheduler.cpp
//         / datalogger.cpp / historyquery.cpp / storagerotator.cpp
// ============================================================

#include "applogic.h"
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <algorithm>

// ============================================================
// AppConfig: app.ini 配置加载/保存
// ============================================================

AppConfig &AppConfig::instance()
{
    static AppConfig cfg;
    return cfg;
}

bool AppConfig::load(const QString &iniPath)
{
    if (!QFileInfo::exists(iniPath))
        return false;

    m_configPath = iniPath;
    QSettings ini(iniPath, QSettings::IniFormat);
    m_validationErrors.clear();

    // QSettings maps the INI section [General] to root-level keys.
    m_general.dataPath = ini.value("dataPath").toString();
    m_general.retentionDays = ini.value("retentionDays", 30).toInt();
    m_general.maxStorageMB = ini.value("maxStorageMB", 512).toInt();
    m_general.pollIntervalMs = ini.value("pollIntervalMs", 1000).toInt();
    m_general.modbusTimeoutMs = ini.value("modbusTimeoutMs", 500).toInt();
    m_general.interSlaveDelayMs = ini.value("interSlaveDelayMs", 50).toInt();
    m_general.temperatureTarget = ini.value("temperatureTarget", 25.0).toDouble();
    m_general.lowerHysteresis = ini.value("lowerHysteresis", 2.0).toDouble();
    m_general.upperHysteresis = ini.value("upperHysteresis", 2.0).toDouble();
    m_general.highVoltageThreshold = ini.value("highVoltageThreshold", 1.0).toDouble();
    m_general.relaySwitchIntervalSec = ini.value("relaySwitchIntervalSec", 10).toInt();
    m_general.recordIntervalSec = ini.value("recordIntervalSec", 1).toInt();

    m_ports.clear();
    for (int i = 0; i < 2; ++i) {
        const QString group = QString("Port%1").arg(i);
        ini.beginGroup(group);
        if (!ini.contains("device"))
            { ini.endGroup(); continue; }

        PortConfig p;
        p.enabled = ini.value("enabled", false).toBool();
        p.name = ini.value("name", group).toString();
        p.device = ini.value("device").toString();
        p.baudRate = ini.value("baudRate", 19200).toInt();
        p.dataBits = ini.value("dataBits", 8).toInt();
        p.parity = ini.value("parity", "N").toString().at(0).toLatin1();
        p.stopBits = ini.value("stopBits", 1).toInt();
        p.frameDelayMs = ini.value("frameDelayMs", 5).toInt();
        m_ports.append(p);
        ini.endGroup();
    }

    return true;
}

bool AppConfig::save(const QString &iniPath) const
{
    QSettings ini(iniPath, QSettings::IniFormat);

    // Keep these as root-level keys so they are written to [General].
    ini.setValue("dataPath", m_general.dataPath);
    ini.setValue("retentionDays", m_general.retentionDays);
    ini.setValue("maxStorageMB", m_general.maxStorageMB);
    ini.setValue("pollIntervalMs", m_general.pollIntervalMs);
    ini.setValue("modbusTimeoutMs", m_general.modbusTimeoutMs);
    ini.setValue("interSlaveDelayMs", m_general.interSlaveDelayMs);
    ini.setValue("temperatureTarget", m_general.temperatureTarget);
    ini.setValue("lowerHysteresis", m_general.lowerHysteresis);
    ini.setValue("upperHysteresis", m_general.upperHysteresis);
    ini.setValue("highVoltageThreshold", m_general.highVoltageThreshold);
    ini.setValue("relaySwitchIntervalSec", m_general.relaySwitchIntervalSec);
    ini.setValue("recordIntervalSec", m_general.recordIntervalSec);

    for (int i = 0; i < m_ports.size(); ++i) {
        const PortConfig &p = m_ports.at(i);
        ini.beginGroup(QString("Port%1").arg(i));
        ini.setValue("enabled", p.enabled);
        ini.setValue("name", p.name);
        ini.setValue("device", p.device);
        ini.setValue("baudRate", p.baudRate);
        ini.setValue("dataBits", p.dataBits);
        ini.setValue("parity", QString(p.parity));
        ini.setValue("stopBits", p.stopBits);
        ini.setValue("frameDelayMs", p.frameDelayMs);
        ini.endGroup();
    }

    ini.sync();
    return ini.status() == QSettings::NoError;
}

// ============================================================
// DeviceManager: 逻辑设备状态管理
// ============================================================

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
}

void DeviceManager::clearDiscoveredDevices()
{
    m_devices.clear();
    emit devicesChanged();
}

QList<DeviceState> DeviceManager::allDevices() const
{
    QList<DeviceState> devices = m_devices.values();
    std::sort(devices.begin(), devices.end(),
              [](const DeviceState &a, const DeviceState &b) {
        if (a.key.portIndex != b.key.portIndex)
            return a.key.portIndex < b.key.portIndex;
        return a.key.slaveId < b.key.slaveId;
    });
    return devices;
}

DeviceState DeviceManager::device(const DeviceProfile::DeviceKey &key) const
{
    return m_devices.value(key);
}

bool DeviceManager::hasDevice(const DeviceProfile::DeviceKey &key) const
{
    return m_devices.contains(key);
}

void DeviceManager::setSelected(const DeviceProfile::DeviceKey &key, bool selected)
{
    if (!m_devices.contains(key))
        return;
    m_devices[key].selected = selected;
    emit deviceUpdated(key);
}

void DeviceManager::setSelectedAll(bool selected)
{
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        it.value().selected = selected;
        emit deviceUpdated(it.key());
    }
}

QList<DeviceState> DeviceManager::selectedDevices() const
{
    QList<DeviceState> list = allDevices();
    for (auto it = list.begin(); it != list.end(); ) {
        if (!it->selected)
            it = list.erase(it);
        else
            ++it;
    }
    return list;
}

void DeviceManager::updateDeviceData(const DeviceProfile::DeviceKey &key,
                                      const QMap<QString, QVariant> &values,
                                      bool online)
{
    if (!m_devices.contains(key)) {
        if (!online)
            return;
        int deviceCountOnPort = 0;
        for (const DeviceState &state : m_devices) {
            if (state.key.portIndex == key.portIndex)
                ++deviceCountOnPort;
        }
        if (deviceCountOnPort >= 16)
            return;

        DeviceState discovered;
        discovered.key = key;
        discovered.name = QString("P%1-ID%2").arg(key.portIndex).arg(key.slaveId);
        discovered.type = DeviceProfile::PcbBoard;
        discovered.selected = true;
        m_devices.insert(key, discovered);
        emit devicesChanged();
    }

    DeviceState &s = m_devices[key];
    s.online = online;
    s.lastUpdate = QDateTime::currentDateTime();
    if (online && !values.isEmpty())
        s.values = values;
    else
        s.lastError = QString::fromUtf8("通信超时/离线");

    emit deviceUpdated(key);
}

// ============================================================
// DataLogger: 按日期分文件的 CSV 数据记录
// ============================================================

DataLogger::DataLogger(QObject *parent)
    : QObject(parent)
{
}

void DataLogger::setDataPath(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    m_dataPath = path;
    QDir().mkpath(path);
}

QString DataLogger::csvFilePathForDate(const QDate &date) const
{
    return m_dataPath + QDir::separator()
         + date.toString("yyyy-MM-dd") + ".csv";
}

void DataLogger::ensureHeader(QTextStream &out, const QString &filePath)
{
    QFile f(filePath);
    if (f.exists() && f.size() > 0)
        return;

    out << "timestamp,port,slave_id,device_name,device_type,data\n";
}

void DataLogger::appendRecord(const DeviceProfile::DeviceKey &key,
                               const QString &deviceName,
                               DeviceProfile::DeviceType type,
                               const QMap<QString, QVariant> &values)
{
    QMutexLocker lock(&m_mutex);
    if (m_dataPath.isEmpty())
        return;

    const QString filePath = csvFilePathForDate(QDate::currentDate());
    QFile file(filePath);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        emit logError(QString::fromUtf8("无法写入日志: %1").arg(filePath));
        return;
    }

    QTextStream out(&file);
    ensureHeader(out, filePath);

    QJsonObject json;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        json[it.key()] = QJsonValue::fromVariant(it.value());

    const QDateTime timestamp = QDateTime::currentDateTime();
    const QString line = QString("%1,%2,%3,%4,%5,%6\n")
        .arg(timestamp.toString("yyyy-MM-dd hh:mm:ss"))
        .arg(key.portIndex)
        .arg(key.slaveId)
        .arg(deviceName)
        .arg(DeviceProfile::deviceTypeToString(type))
        .arg(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));

    out << line;
    file.close();
    lock.unlock();
    emit recordAppended(timestamp, key, deviceName, type, values);
}

// ============================================================
// HistoryQuery: 历史数据查询 (读取 CSV 文件)
// ============================================================

HistoryQuery::HistoryQuery(QObject *parent)
    : QObject(parent)
{
}

void HistoryQuery::setDataPath(const QString &path)
{
    m_dataPath = path;
}

QVector<HistoryQuery::Record> HistoryQuery::query(const Filter &filter) const
{
    QVector<Record> results;
    if (m_dataPath.isEmpty() || !filter.dateFrom.isValid())
        return results;

    QDate d = filter.dateFrom;
    const QDate end = filter.dateTo.isValid() ? filter.dateTo : filter.dateFrom;

    while (d <= end) {
        const QString filePath = m_dataPath + QDir::separator()
                                 + d.toString("yyyy-MM-dd") + ".csv";
        results += parseFile(filePath, filter);
        d = d.addDays(1);
    }
    return results;
}

QVector<HistoryQuery::DeviceInfo> HistoryQuery::availableDevices() const
{
    QMap<QPair<int, int>, DeviceInfo> devices;
    const QFileInfoList files = QDir(m_dataPath).entryInfoList(
        QStringList() << "*.csv", QDir::Files, QDir::Name);
    for (const QFileInfo &fileInfo : files) {
        QFile file(fileInfo.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QTextStream in(&file);
        in.readLine();
        while (!in.atEnd()) {
            const QString line = in.readLine();
            const int p1 = line.indexOf(',');
            const int p2 = line.indexOf(',', p1 + 1);
            const int p3 = line.indexOf(',', p2 + 1);
            const int p4 = line.indexOf(',', p3 + 1);
            if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0)
                continue;
            DeviceInfo info;
            info.portIndex = line.mid(p1 + 1, p2 - p1 - 1).toInt();
            info.slaveId = line.mid(p2 + 1, p3 - p2 - 1).toInt();
            info.name = line.mid(p3 + 1, p4 - p3 - 1);
            devices.insert(QPair<int, int>(info.portIndex, info.slaveId), info);
        }
    }
    return devices.values().toVector();
}

QVector<HistoryQuery::Record> HistoryQuery::parseFile(const QString &filePath,
                                                       const Filter &filter) const
{
    QVector<Record> results;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return results;

    QTextStream in(&file);
    if (in.atEnd())
        return results;

    in.readLine(); // skip header

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty())
            continue;

        // CSV: 前 5 列固定，data 列可能含逗号 (JSON 通常无逗号空格)
        const int p1 = line.indexOf(',');
        const int p2 = line.indexOf(',', p1 + 1);
        const int p3 = line.indexOf(',', p2 + 1);
        const int p4 = line.indexOf(',', p3 + 1);
        const int p5 = line.indexOf(',', p4 + 1);
        if (p5 < 0)
            continue;

        Record rec;
        rec.timestamp = QDateTime::fromString(line.left(p1), "yyyy-MM-dd hh:mm:ss");
        rec.portIndex = line.mid(p1 + 1, p2 - p1 - 1).toInt();
        rec.slaveId = line.mid(p2 + 1, p3 - p2 - 1).toInt();
        rec.deviceName = line.mid(p3 + 1, p4 - p3 - 1);
        rec.deviceType = DeviceProfile::deviceTypeFromString(
            line.mid(p4 + 1, p5 - p4 - 1));

        if (filter.portIndex >= 0 && rec.portIndex != filter.portIndex)
            continue;
        if (filter.slaveId >= 0 && rec.slaveId != filter.slaveId)
            continue;
        if (!filter.deviceType.isEmpty()
            && DeviceProfile::deviceTypeToString(rec.deviceType) != filter.deviceType)
            continue;

        const QString jsonStr = line.mid(p5 + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
                rec.values[it.key()] = it.value().toVariant();
        }

        results.append(rec);
    }
    return results;
}

// ============================================================
// StorageRotator: 存储清理轮转
// ============================================================

StorageRotator::StorageRotator(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    connect(m_timer, &QTimer::timeout, this, &StorageRotator::runCleanup);
}

void StorageRotator::setDataPath(const QString &path)
{
    m_dataPath = path;
}

void StorageRotator::setRetentionDays(int days)
{
    m_retentionDays = days;
}

void StorageRotator::setMaxStorageMB(int mb)
{
    m_maxStorageMB = mb;
}

void StorageRotator::start(int checkIntervalMs)
{
    runCleanup();
    m_timer->start(checkIntervalMs);
}

void StorageRotator::stop()
{
    m_timer->stop();
}

void StorageRotator::runCleanup()
{
    if (m_dataPath.isEmpty())
        return;

    QDir dir(m_dataPath);
    const QFileInfoList files = dir.entryInfoList(
        QStringList() << "*.csv", QDir::Files, QDir::Time | QDir::Reversed);

    int removed = 0;
    qint64 freed = 0;
    const QDate cutoff = QDate::currentDate().addDays(-m_retentionDays);

    // 1) 按保留天数删除
    for (const QFileInfo &fi : files) {
        const QDate fileDate = QDate::fromString(
            fi.baseName(), "yyyy-MM-dd");
        if (fileDate.isValid() && fileDate < cutoff) {
            freed += fi.size();
            QFile::remove(fi.absoluteFilePath());
            ++removed;
        }
    }

    // 2) 按总大小上限删除最旧文件
    if (m_maxStorageMB > 0) {
        const qint64 maxBytes = static_cast<qint64>(m_maxStorageMB) * 1024 * 1024;
        QFileInfoList remaining = dir.entryInfoList(
            QStringList() << "*.csv", QDir::Files, QDir::Time | QDir::Reversed);

        qint64 total = 0;
        for (const QFileInfo &fi : remaining)
            total += fi.size();

        for (const QFileInfo &fi : remaining) {
            if (total <= maxBytes)
                break;
            total -= fi.size();
            freed += fi.size();
            QFile::remove(fi.absoluteFilePath());
            ++removed;
        }
    }

    if (removed > 0)
        emit cleanupDone(removed, freed);
}

// ============================================================
// PollScheduler: 轮询调度器
// ============================================================

PollScheduler::PollScheduler(DeviceManager *deviceMgr,
                             DataLogger *logger,
                             QObject *parent)
    : QObject(parent)
    , m_deviceMgr(deviceMgr)
    , m_logger(logger)
{
}

PollScheduler::~PollScheduler()
{
    stop();
}

bool PollScheduler::start()
{
    const AppConfig &cfg = AppConfig::instance();

    for (int i = 0; i < cfg.ports().size(); ++i) {
        const AppConfig::PortConfig &pc = cfg.ports().at(i);
        if (pc.enabled)
            setupPortWorker(i, pc);
    }

    refreshPollTasks();

    for (auto it = m_ports.begin(); it != m_ports.end(); ++it) {
        PortHandle &h = it.value();
        QMetaObject::invokeMethod(h.worker, "startWork", Qt::QueuedConnection);
        QMetaObject::invokeMethod(h.worker, "startPolling", Qt::QueuedConnection,
                                  Q_ARG(int, cfg.general().pollIntervalMs));
    }

    return !m_ports.isEmpty();
}

void PollScheduler::stop()
{
    for (auto it = m_ports.begin(); it != m_ports.end(); ++it) {
        PortHandle &h = it.value();
        if (h.worker)
            QMetaObject::invokeMethod(h.worker, "stopWork", Qt::QueuedConnection);
        if (h.thread) {
            h.thread->quit();
            h.thread->wait(3000);
        }
    }
    m_ports.clear();
}

void PollScheduler::refreshPollTasks()
{
    const QList<DeviceState> selected = m_deviceMgr->selectedDevices();

    QHash<int, QVector<SerialPortWorker::PollTask>> tasksByPort;
    for (const DeviceState &ds : selected) {
        SerialPortWorker::PollTask task;
        task.deviceKey = ds.key;
        task.deviceType = ds.type;
        task.regMap = DeviceProfile::defaultRegisterMap(ds.type);
        tasksByPort[ds.key.portIndex].append(task);
    }

    for (auto it = m_ports.begin(); it != m_ports.end(); ++it) {
        const int portIndex = it.key();
        SerialPortWorker *worker = it.value().worker;
        const QVector<SerialPortWorker::PollTask> tasks = tasksByPort.value(portIndex);
        QMetaObject::invokeMethod(worker, [worker, tasks]() {
            worker->setPollTasks(tasks);
        }, Qt::QueuedConnection);
    }
}

void PollScheduler::rescanDevices()
{
    m_lastLogTimes.clear();
    m_deviceMgr->clearDiscoveredDevices();
    refreshPollTasks();
    for (auto it = m_ports.begin(); it != m_ports.end(); ++it) {
        QMetaObject::invokeMethod(it.value().worker, "restartDiscovery",
                                  Qt::QueuedConnection);
    }
}

void PollScheduler::writeToDevice(const DeviceProfile::DeviceKey &key,
                                   const QMap<QString, QVariant> &fields)
{
    if (!m_ports.contains(key.portIndex)) {
        emit writeCompleted(key, false, QString::fromUtf8("端口未启用"));
        return;
    }

    const DeviceState ds = m_deviceMgr->device(key);
    SerialPortWorker::WriteTask task;
    task.deviceKey = key;
    task.deviceType = ds.type;
    task.fields = fields;

    SerialPortWorker *worker = m_ports[key.portIndex].worker;
    QMetaObject::invokeMethod(worker, [worker, task]() {
        worker->enqueueWrite(task);
    }, Qt::QueuedConnection);
}

void PollScheduler::setupPortWorker(int portIndex, const AppConfig::PortConfig &portCfg)
{
    QThread *thread = new QThread(this);
    SerialPortWorker::PortSettings settings;
    settings.portIndex = portIndex;
    settings.portName = portCfg.name;
    settings.device = portCfg.device;
    settings.baudRate = portCfg.baudRate;
    settings.dataBits = portCfg.dataBits;
    settings.frameDelayMs = portCfg.frameDelayMs;
    settings.timeoutMs = AppConfig::instance().general().modbusTimeoutMs;
    settings.interSlaveDelayMs = AppConfig::instance().general().interSlaveDelayMs;

    switch (portCfg.parity) {
    case 'E': settings.parity = QSerialPort::EvenParity; break;
    case 'O': settings.parity = QSerialPort::OddParity; break;
    default:  settings.parity = QSerialPort::NoParity; break;
    }
    settings.stopBits = (portCfg.stopBits == 2)
        ? QSerialPort::TwoStop : QSerialPort::OneStop;

    SerialPortWorker *worker = new SerialPortWorker(settings);
    worker->moveToThread(thread);

    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &SerialPortWorker::deviceDataReady,
            this, &PollScheduler::onDeviceDataReady, Qt::QueuedConnection);
    connect(worker, &SerialPortWorker::writeFinished,
            this, &PollScheduler::onWriteFinished, Qt::QueuedConnection);
    connect(worker, &SerialPortWorker::portError,
            this, [this](int, const QString &err) {
                emit schedulerError(err);
            });

    thread->start();

    PortHandle handle;
    handle.thread = thread;
    handle.worker = worker;
    m_ports.insert(portIndex, handle);
}

void PollScheduler::onDeviceDataReady(DeviceProfile::DeviceKey key,
                                       const QMap<QString, QVariant> &values,
                                       bool online)
{
    const bool discovered = online && !m_deviceMgr->hasDevice(key);
    m_deviceMgr->updateDeviceData(key, values, online);
    if (discovered && m_deviceMgr->hasDevice(key))
        refreshPollTasks();

    const QDateTime now = QDateTime::currentDateTime();
    const int recordInterval = qMax(1, AppConfig::instance().general().recordIntervalSec);
    if (online && !values.isEmpty() && m_logger
        && (!m_lastLogTimes.contains(key)
            || m_lastLogTimes.value(key).secsTo(now) >= recordInterval)) {
        const DeviceState ds = m_deviceMgr->device(key);
        m_logger->appendRecord(key, ds.name, ds.type, values);
        m_lastLogTimes[key] = now;
    }
}

void PollScheduler::onWriteFinished(DeviceProfile::DeviceKey key, bool success,
                                     const QString &error)
{
    emit writeCompleted(key, success, error);
}
