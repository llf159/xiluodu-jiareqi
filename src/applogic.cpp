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
#include <QStorageInfo>
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
    m_general.maxStorageMB = ini.value("maxStorageMB", 12288).toInt();
    m_general.pollIntervalMs = ini.value("pollIntervalMs", 1000).toInt();
    m_general.modbusTimeoutMs = ini.value("modbusTimeoutMs", 500).toInt();
    m_general.interSlaveDelayMs = ini.value("interSlaveDelayMs", 50).toInt();
    m_general.temperatureTarget = ini.value("temperatureTarget", 25.0).toDouble();
    m_general.temperatureTargetSource =
        ini.value("temperatureTargetSource", "pt100").toString().toLower();
    if (m_general.temperatureTargetSource != "fixed")
        m_general.temperatureTargetSource = "pt100";
    m_general.temperatureControlMode =
        ini.value("temperatureControlMode", "threshold").toString().toLower();
    if (m_general.temperatureControlMode != "pid"
        && m_general.temperatureControlMode != "dew_point")
        m_general.temperatureControlMode = "threshold";
    QList<double> thresholdStages = {
        ini.value("thresholdSingleStageDelta", 0.3).toDouble(),
        ini.value("thresholdSecondStageDelta", 1.5).toDouble(),
        ini.value("thresholdDualStageDelta", 3.0).toDouble()
    };
    std::sort(thresholdStages.begin(), thresholdStages.end());
    m_general.thresholdSingleStageDelta = qBound(0.0, thresholdStages.at(0), 30.0);
    m_general.thresholdSecondStageDelta = qBound(
        m_general.thresholdSingleStageDelta, thresholdStages.at(1), 30.0);
    m_general.thresholdDualStageDelta = qBound(
        m_general.thresholdSecondStageDelta, thresholdStages.at(2), 30.0);
    m_general.thresholdHysteresis = qBound(
        0.0, ini.value("thresholdHysteresis", 0.2).toDouble(),
        qMin((m_general.thresholdSecondStageDelta
              - m_general.thresholdSingleStageDelta) / 2.0,
             (m_general.thresholdDualStageDelta
              - m_general.thresholdSecondStageDelta) / 2.0));
    QList<double> dewPointMargins = {
        ini.value("dewPointSingleStageMargin", 3.0).toDouble(),
        ini.value("dewPointSecondStageMargin", 2.0).toDouble(),
        ini.value("dewPointDualStageMargin", 1.0).toDouble()
    };
    std::sort(dewPointMargins.begin(), dewPointMargins.end());
    m_general.dewPointSingleStageMargin = qBound(
        0.0, dewPointMargins.at(2), 20.0);
    m_general.dewPointSecondStageMargin = qBound(
        0.0, dewPointMargins.at(1), m_general.dewPointSingleStageMargin);
    m_general.dewPointDualStageMargin = qBound(
        0.0, dewPointMargins.at(0), m_general.dewPointSecondStageMargin);
    m_general.dewPointHysteresis = qBound(
        0.0, ini.value("dewPointHysteresis", 0.2).toDouble(),
        qMin((m_general.dewPointSingleStageMargin
              - m_general.dewPointSecondStageMargin) / 2.0,
             (m_general.dewPointSecondStageMargin
              - m_general.dewPointDualStageMargin) / 2.0));
    m_general.humidityTemperatureLimitDelta = qBound(
        0.0, ini.value("humidityTemperatureLimitDelta", 5.0).toDouble(), 30.0);
    m_general.pidKp = ini.value("pidKp", 12.0).toDouble();
    m_general.pidKi = ini.value("pidKi", 0.15).toDouble();
    m_general.pidKd = ini.value("pidKd", 0.0).toDouble();
    m_general.pidSingleStagePercent =
        ini.value("pidSingleStagePercent", 10.0).toDouble();
    m_general.pidSecondStagePercent =
        ini.value("pidSecondStagePercent", 35.0).toDouble();
    m_general.pidDualStagePercent =
        ini.value("pidDualStagePercent", 60.0).toDouble();
    m_general.pidFirstStageOutput =
        ini.value("pidFirstStageOutput", "ot3").toString().toLower();
    if (m_general.pidFirstStageOutput != "ot4")
        m_general.pidFirstStageOutput = "ot3";
    auto validSpareMode = [](const QString &mode) {
        return mode == "off" || mode == "manual"
            || mode == "auto" || mode == "alarm"
            || mode == "follow_ot3" || mode == "follow_ot4";
    };
    m_general.spareOt01Mode = ini.value("spareOt01Mode", "off").toString();
    m_general.spareOt02Mode = ini.value("spareOt02Mode", "off").toString();
    m_general.spareOt05Mode = ini.value("spareOt05Mode", "off").toString();
    m_general.spareOt06Mode = ini.value("spareOt06Mode", "off").toString();
    for (QString *mode : { &m_general.spareOt01Mode, &m_general.spareOt02Mode,
                           &m_general.spareOt05Mode, &m_general.spareOt06Mode }) {
        if (!validSpareMode(*mode))
            *mode = "off";
    }
    m_general.reservedInputMode =
        ini.value("reservedInputMode", "monitor").toString();
    if (m_general.reservedInputMode != "interlock_high"
        && m_general.reservedInputMode != "interlock_low")
        m_general.reservedInputMode = "monitor";
    m_general.highVoltageDetectionMode =
        ini.value("highVoltageDetectionMode", "analog").toString().toLower();
    if (m_general.highVoltageDetectionMode != "digital")
        m_general.highVoltageDetectionMode = "analog";
    m_general.highVoltageDigitalTrigger =
        ini.value("highVoltageDigitalTrigger", 1).toInt();
    if (m_general.highVoltageDigitalTrigger != 0
        && m_general.highVoltageDigitalTrigger != 1)
        m_general.highVoltageDigitalTrigger = 1;
    m_general.pidKp = qBound(0.0, m_general.pidKp, 100.0);
    m_general.pidKi = qBound(0.0, m_general.pidKi, 10.0);
    m_general.pidKd = qBound(0.0, m_general.pidKd, 100.0);
    m_general.pidSingleStagePercent =
        qBound(0.0, m_general.pidSingleStagePercent, 100.0);
    m_general.pidSecondStagePercent =
        qBound(m_general.pidSingleStagePercent,
               m_general.pidSecondStagePercent, 100.0);
    m_general.pidDualStagePercent =
        qBound(m_general.pidSecondStagePercent,
               m_general.pidDualStagePercent, 100.0);
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

    for (const char *obsoleteKey : {
             "retentionDays", "selfCheckGraceSec", "sensorRecoverySamples",
             "sensorTemperatureMin", "sensorTemperatureMax",
             "sensorHumidityMin", "sensorHumidityMax",
             "lowerHysteresis", "upperHysteresis",
             "humidityControlMode", "humidityMaxHeatingStage",
             "dewPointMargin", "dewPointRecoveryHysteresis" })
        ini.remove(obsoleteKey);

    // Keep these as root-level keys so they are written to [General].
    ini.setValue("dataPath", m_general.dataPath);
    ini.setValue("maxStorageMB", m_general.maxStorageMB);
    ini.setValue("pollIntervalMs", m_general.pollIntervalMs);
    ini.setValue("modbusTimeoutMs", m_general.modbusTimeoutMs);
    ini.setValue("interSlaveDelayMs", m_general.interSlaveDelayMs);
    ini.setValue("temperatureTarget", m_general.temperatureTarget);
    ini.setValue("temperatureTargetSource", m_general.temperatureTargetSource);
    ini.setValue("temperatureControlMode", m_general.temperatureControlMode);
    ini.setValue("thresholdSingleStageDelta", m_general.thresholdSingleStageDelta);
    ini.setValue("thresholdSecondStageDelta", m_general.thresholdSecondStageDelta);
    ini.setValue("thresholdDualStageDelta", m_general.thresholdDualStageDelta);
    ini.setValue("thresholdHysteresis", m_general.thresholdHysteresis);
    ini.setValue("dewPointSingleStageMargin",
                 m_general.dewPointSingleStageMargin);
    ini.setValue("dewPointSecondStageMargin",
                 m_general.dewPointSecondStageMargin);
    ini.setValue("dewPointDualStageMargin",
                 m_general.dewPointDualStageMargin);
    ini.setValue("dewPointHysteresis", m_general.dewPointHysteresis);
    ini.setValue("humidityTemperatureLimitDelta",
                 m_general.humidityTemperatureLimitDelta);
    ini.setValue("pidKp", m_general.pidKp);
    ini.setValue("pidKi", m_general.pidKi);
    ini.setValue("pidKd", m_general.pidKd);
    ini.setValue("pidSingleStagePercent", m_general.pidSingleStagePercent);
    ini.setValue("pidSecondStagePercent", m_general.pidSecondStagePercent);
    ini.setValue("pidDualStagePercent", m_general.pidDualStagePercent);
    ini.setValue("pidFirstStageOutput", m_general.pidFirstStageOutput);
    ini.setValue("spareOt01Mode", m_general.spareOt01Mode);
    ini.setValue("spareOt02Mode", m_general.spareOt02Mode);
    ini.setValue("spareOt05Mode", m_general.spareOt05Mode);
    ini.setValue("spareOt06Mode", m_general.spareOt06Mode);
    ini.setValue("reservedInputMode", m_general.reservedInputMode);
    ini.setValue("highVoltageDetectionMode", m_general.highVoltageDetectionMode);
    ini.setValue("highVoltageDigitalTrigger", m_general.highVoltageDigitalTrigger);
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

    out << "timestamp,port,slave_id,device_name,"
           "th1_temp_c,th1_humi_pct,th2_temp_c,th2_humi_pct,"
           "th3_temp_c,th3_humi_pct,pt1_temp_c,pt2_temp_c,"
           "external_voltage_v,reserved_input,"
           "ot01,ot02,ot03,ot04,ot05,ot06\n";
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
    bool legacySchema = false;
    if (QFileInfo(filePath).size() > 0) {
        QFile existing(filePath);
        if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString header = QString::fromUtf8(existing.readLine()).trimmed();
            legacySchema =
                header == "timestamp,port,slave_id,device_name,device_type,data";
        }
    }
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

    auto engineering = [&values](const QString &field) {
        return values.contains(field)
            ? QString::number(values.value(field).toInt() / 10.0, 'f', 1)
            : QString();
    };
    auto raw = [&values](const QString &field) {
        return values.contains(field) ? values.value(field).toString() : QString();
    };
    const QDateTime timestamp = QDateTime::currentDateTime();
    QStringList columns;
    columns << timestamp.toString(Qt::ISODateWithMs)
            << QString::number(key.portIndex)
            << QString::number(key.slaveId)
            << deviceName
            << engineering("th1_temp") << engineering("th1_humi")
            << engineering("th2_temp") << engineering("th2_humi")
            << engineering("th3_temp") << engineering("th3_humi")
            << engineering("pt1_temp") << engineering("pt2_temp")
            << engineering("external_voltage") << raw("reserved");
    for (int i = 1; i <= 6; ++i)
        columns << raw(QString("ot%1").arg(i, 2, 10, QChar('0')));
    QString line;
    const QString jsonText =
        QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact));
    if (legacySchema) {
        line = QString("%1,%2,%3,%4,%5,%6\n")
            .arg(timestamp.toString("yyyy-MM-dd hh:mm:ss"))
            .arg(key.portIndex)
            .arg(key.slaveId)
            .arg(deviceName)
            .arg(DeviceProfile::deviceTypeToString(type))
            .arg(jsonText);
    } else {
        line = columns.join(',') + '\n';
    }

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
        const QStringList header = in.readLine().trimmed().split(',');
        const int portColumn = header.indexOf("port");
        const int slaveColumn = header.indexOf("slave_id");
        const int nameColumn = header.indexOf("device_name");
        if (portColumn < 0 || slaveColumn < 0 || nameColumn < 0)
            continue;
        while (!in.atEnd()) {
            const QStringList columns = in.readLine().trimmed().split(',');
            const int required = qMax(portColumn, qMax(slaveColumn, nameColumn));
            if (columns.size() <= required)
                continue;
            DeviceInfo info;
            info.portIndex = columns.at(portColumn).toInt();
            info.slaveId = columns.at(slaveColumn).toInt();
            info.name = columns.at(nameColumn);
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

    const QString header = in.readLine();
    const QStringList headerFields = header.split(',');
    int dataColumn = headerFields.indexOf("data_json");
    if (dataColumn < 0)
        dataColumn = headerFields.indexOf("data");
    const int timestampColumn = headerFields.indexOf("timestamp");
    const int portColumn = headerFields.indexOf("port");
    const int slaveColumn = headerFields.indexOf("slave_id");
    const int nameColumn = headerFields.indexOf("device_name");
    if (timestampColumn < 0 || portColumn < 0
        || slaveColumn < 0 || nameColumn < 0)
        return results;

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty())
            continue;

        Record rec;
        rec.deviceType = DeviceProfile::PcbBoard;
        if (dataColumn >= 0) {
            QStringList fixed;
            int fieldStart = 0;
            int comma = -1;
            for (int i = 0; i < dataColumn; ++i) {
                comma = line.indexOf(',', fieldStart);
                if (comma < 0)
                    break;
                fixed.append(line.mid(fieldStart, comma - fieldStart));
                fieldStart = comma + 1;
            }
            if (fixed.size() != dataColumn
                || fixed.size() <= qMax(nameColumn, slaveColumn))
                continue;
            rec.timestamp =
                QDateTime::fromString(fixed.at(timestampColumn), Qt::ISODateWithMs);
            if (!rec.timestamp.isValid())
                rec.timestamp = QDateTime::fromString(
                    fixed.at(timestampColumn), "yyyy-MM-dd hh:mm:ss");
            rec.portIndex = fixed.at(portColumn).toInt();
            rec.slaveId = fixed.at(slaveColumn).toInt();
            rec.deviceName = fixed.at(nameColumn);
            const int typeColumn = headerFields.indexOf("device_type");
            if (typeColumn >= 0 && typeColumn < fixed.size())
                rec.deviceType =
                    DeviceProfile::deviceTypeFromString(fixed.at(typeColumn));

            const QString jsonStr = line.mid(fieldStart);
            const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
            if (doc.isObject()) {
                const QJsonObject obj = doc.object();
                for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
                    rec.values[it.key()] = it.value().toVariant();
            }
        } else {
            const QStringList columns = line.split(',');
            if (columns.size() < headerFields.size())
                continue;
            rec.timestamp =
                QDateTime::fromString(columns.at(timestampColumn), Qt::ISODateWithMs);
            if (!rec.timestamp.isValid())
                rec.timestamp = QDateTime::fromString(
                    columns.at(timestampColumn), "yyyy-MM-dd hh:mm:ss");
            rec.portIndex = columns.at(portColumn).toInt();
            rec.slaveId = columns.at(slaveColumn).toInt();
            rec.deviceName = columns.at(nameColumn);

            const QMap<QString, QString> engineeringFields = {
                { "th1_temp_c", "th1_temp" }, { "th1_humi_pct", "th1_humi" },
                { "th2_temp_c", "th2_temp" }, { "th2_humi_pct", "th2_humi" },
                { "th3_temp_c", "th3_temp" }, { "th3_humi_pct", "th3_humi" },
                { "pt1_temp_c", "pt1_temp" }, { "pt2_temp_c", "pt2_temp" },
                { "external_voltage_v", "external_voltage" }
            };
            for (auto it = engineeringFields.constBegin();
                 it != engineeringFields.constEnd(); ++it) {
                const int column = headerFields.indexOf(it.key());
                if (column >= 0 && column < columns.size()
                    && !columns.at(column).isEmpty())
                    rec.values[it.value()] = qRound(columns.at(column).toDouble() * 10.0);
            }
            const QStringList rawFields = {
                "hv_input", "reserved_input",
                "ot01", "ot02", "ot03", "ot04", "ot05", "ot06"
            };
            for (const QString &columnName : rawFields) {
                const int column = headerFields.indexOf(columnName);
                if (column < 0 || column >= columns.size()
                    || columns.at(column).isEmpty())
                    continue;
                const QString valueField =
                    columnName == "reserved_input" ? "reserved" : columnName;
                rec.values[valueField] = columns.at(column).toInt();
            }
        }

        if (filter.portIndex >= 0 && rec.portIndex != filter.portIndex)
            continue;
        if (filter.slaveId >= 0 && rec.slaveId != filter.slaveId)
            continue;
        if (!filter.deviceType.isEmpty()
            && DeviceProfile::deviceTypeToString(rec.deviceType) != filter.deviceType)
            continue;

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

StorageRotator::StorageStatus StorageRotator::storageStatus() const
{
    StorageStatus status;
    if (m_dataPath.isEmpty()) {
        status.error = QString::fromUtf8("未配置数据目录");
        return status;
    }

    QDir().mkpath(m_dataPath);
    QStorageInfo storage(m_dataPath);
    storage.refresh();
    if (!storage.isValid() || !storage.isReady()) {
        status.error = QString::fromUtf8("存储设备不可用");
        return status;
    }
    status.ready = true;
    status.bytesTotal = storage.bytesTotal();
    status.bytesAvailable = storage.bytesAvailable();
    const QFileInfoList files = QDir(m_dataPath).entryInfoList(
        QStringList() << "*.csv", QDir::Files, QDir::Name);
    status.fileCount = files.size();
    for (const QFileInfo &file : files) {
        status.logBytes += file.size();
        const QDate date = QDate::fromString(file.baseName(), "yyyy-MM-dd");
        if (!date.isValid())
            continue;
        if (!status.oldestDate.isValid() || date < status.oldestDate)
            status.oldestDate = date;
        if (!status.newestDate.isValid() || date > status.newestDate)
            status.newestDate = date;
    }
    return status;
}

StorageRotator::DeleteResult StorageRotator::previewDeleteBefore(
    const QDate &cutoff) const
{
    DeleteResult result;
    if (m_dataPath.isEmpty() || !cutoff.isValid()) {
        result.error = QString::fromUtf8("删除日期或数据目录无效");
        return result;
    }
    result.valid = true;
    const QFileInfoList files = QDir(m_dataPath).entryInfoList(
        QStringList() << "*.csv", QDir::Files, QDir::Name);
    for (const QFileInfo &file : files) {
        const QDate date = QDate::fromString(file.baseName(), "yyyy-MM-dd");
        if (date.isValid() && date < cutoff) {
            ++result.files;
            result.bytes += file.size();
        }
    }
    return result;
}

StorageRotator::DeleteResult StorageRotator::deleteBefore(const QDate &cutoff)
{
    DeleteResult result = previewDeleteBefore(cutoff);
    if (!result.valid)
        return result;
    result.files = 0;
    result.bytes = 0;
    const QFileInfoList files = QDir(m_dataPath).entryInfoList(
        QStringList() << "*.csv", QDir::Files, QDir::Name);
    for (const QFileInfo &file : files) {
        const QDate date = QDate::fromString(file.baseName(), "yyyy-MM-dd");
        if (!date.isValid() || date >= cutoff)
            continue;
        const qint64 size = file.size();
        if (QFile::remove(file.absoluteFilePath())) {
            ++result.files;
            result.bytes += size;
        } else {
            result.valid = false;
            result.error = QString::fromUtf8("部分文件删除失败：%1").arg(file.fileName());
        }
    }
    if (result.files > 0)
        emit cleanupDone(result.files, result.bytes);
    return result;
}

void StorageRotator::runCleanup()
{
    if (m_dataPath.isEmpty())
        return;

    QDir dir(m_dataPath);
    int removed = 0;
    qint64 freed = 0;
    // 平时全部保留，仅超过总大小上限时删除最旧文件。
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
            const qint64 size = fi.size();
            if (QFile::remove(fi.absoluteFilePath())) {
                total -= size;
                freed += size;
                ++removed;
            }
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
