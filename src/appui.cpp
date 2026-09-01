#include "appui.h"

#include <QAbstractSpinBox>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

class TouchComboDelegate : public QStyledItemDelegate
{
public:
    explicit TouchComboDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(44);
        return size;
    }

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        painter->save();
        painter->fillRect(option.rect, selected ? QColor("#1976d2")
                                                : hovered ? QColor("#e7f1fb")
                                                          : Qt::white);
        painter->setPen(selected ? Qt::white : QColor("#202020"));
        painter->drawText(option.rect.adjusted(12, 0, -8, 0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(Qt::DisplayRole).toString());
        painter->restore();
    }
};

QFrame *makeCard(QWidget *parent = nullptr)
{
    auto *card = new QFrame(parent);
    card->setObjectName("card");
    card->setFrameShape(QFrame::NoFrame);
    return card;
}

void refreshDynamicStyle(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void configureDeviceCombo(QComboBox *combo)
{
    combo->setItemDelegate(new TouchComboDelegate(combo));
    combo->setMaxVisibleItems(7);
    combo->view()->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    combo->view()->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    combo->view()->verticalScrollBar()->setSingleStep(44);
    combo->view()->setStyleSheet(QString::fromUtf8(
        "QScrollBar:vertical { width: 36px; background: #eeeeee; }"
        "QScrollBar::handle:vertical { background: #666666; min-height: 72px; margin: 2px 4px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));
}

double averageField(const QMap<QString, QVariant> &values,
                    const QStringList &fields)
{
    double sum = 0.0;
    int count = 0;
    for (const QString &field : fields) {
        if (!values.contains(field))
            continue;
        sum += values.value(field).toInt() / 10.0;
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}

void mergeFleetRecord(QVector<HistoryQuery::Record> &aggregates,
                      const HistoryQuery::Record &record)
{
    const QDateTime timestamp = QDateTime::fromString(
        record.timestamp.toString("yyyy-MM-dd hh:mm:ss"), "yyyy-MM-dd hh:mm:ss");
    if (aggregates.isEmpty() || aggregates.last().timestamp != timestamp) {
        HistoryQuery::Record aggregate;
        aggregate.timestamp = timestamp;
        aggregate.portIndex = -1;
        aggregate.slaveId = -1;
        aggregate.deviceName = QString::fromUtf8("全部子板");
        aggregate.values["fleet_count"] = 0;
        aggregates.append(aggregate);
    }

    QMap<QString, QVariant> &values = aggregates.last().values;
    values["fleet_count"] = values.value("fleet_count").toInt() + 1;
    const QList<QPair<QStringList, QString>> metrics = {
        { { "th1_temp", "th2_temp", "th3_temp" }, "th1_temp" },
        { { "th1_humi", "th2_humi", "th3_humi" }, "th1_humi" },
        { { "pt1_temp", "pt2_temp" }, "pt1_temp" }
    };
    for (const auto &metric : metrics) {
        bool present = false;
        for (const QString &field : metric.first) {
            if (record.values.contains(field)) {
                present = true;
                break;
            }
        }
        if (!present)
            continue;
        const int sample = qRound(averageField(record.values, metric.first) * 10.0);
        const QString sumField = "fleet_" + metric.second + "_sum";
        const QString countField = "fleet_" + metric.second + "_count";
        const int sum = values.value(sumField).toInt() + sample;
        const int count = values.value(countField).toInt() + 1;
        values[sumField] = sum;
        values[countField] = count;
        values[metric.second] = qRound(static_cast<double>(sum) / count);
        if (metric.second == "th1_temp") {
            values["fleet_temp_min"] = count == 1
                ? sample : qMin(values.value("fleet_temp_min").toInt(), sample);
            values["fleet_temp_max"] = count == 1
                ? sample : qMax(values.value("fleet_temp_max").toInt(), sample);
        }
    }
}

bool hasHighVoltage(const QMap<QString, QVariant> &values)
{
    return values.contains("external_voltage")
        && values.value("external_voltage").toInt() / 10.0
            > AppConfig::instance().general().highVoltageThreshold;
}

int commandKey(const DeviceProfile::DeviceKey &key)
{
    return key.portIndex * 256 + key.slaveId;
}

QString formatBytes(qint64 bytes)
{
    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = qMax<qint64>(0, bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QString("%1 %2")
        .arg(QString::number(value, 'f', unit == 0 ? 0 : 1))
        .arg(units[unit]);
}

ControlAlgorithm::SensorLimits configuredSensorLimits()
{
    const AppConfig::GeneralConfig &config = AppConfig::instance().general();
    ControlAlgorithm::SensorLimits limits;
    limits.temperatureMin = config.sensorTemperatureMin;
    limits.temperatureMax = config.sensorTemperatureMax;
    limits.humidityMin = config.sensorHumidityMin;
    limits.humidityMax = config.sensorHumidityMax;
    limits.temperatureMaxDeviation = config.sensorTemperatureMaxDeviation;
    limits.humidityMaxDeviation = config.sensorHumidityMaxDeviation;
    return limits;
}

} // namespace

// ============================================================
// DeviceOverviewWidget
// ============================================================

DeviceOverviewWidget::DeviceOverviewWidget(DeviceManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *selectorCard = makeCard(this);
    auto *selector = new QHBoxLayout(selectorCard);
    selector->setContentsMargins(16, 10, 16, 10);
    m_deviceTitle = new QLabel(QString::fromUtf8("当前子板（已发现 0 块）"), selectorCard);
    selector->addWidget(m_deviceTitle);
    m_deviceBox = new QComboBox(selectorCard);
    configureDeviceCombo(m_deviceBox);
    m_deviceBox->setMinimumHeight(42);
    m_deviceBox->setMinimumWidth(210);
    selector->addWidget(m_deviceBox);
    selector->addStretch();
    m_linkState = new QLabel(QString::fromUtf8("等待连接"), selectorCard);
    m_linkState->setObjectName("statusPill");
    selector->addWidget(m_linkState);
    layout->addWidget(selectorCard);

    auto *metrics = new QGridLayout;
    metrics->setSpacing(10);
    const QStringList titles = {
        QString::fromUtf8("温湿度传感器 1"),
        QString::fromUtf8("温湿度传感器 2"),
        QString::fromUtf8("温湿度传感器 3")
    };
    for (int i = 0; i < 3; ++i) {
        auto *card = makeCard(this);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(13, 10, 13, 10);
        auto *title = new QLabel(titles.at(i), card);
        title->setObjectName("metricTitle");
        auto *temperature = new QLabel("--.- ℃", card);
        temperature->setObjectName("metricValue");
        auto *humidity = new QLabel("--.- %RH", card);
        humidity->setObjectName("metricSubValue");
        cardLayout->addWidget(title);
        cardLayout->addWidget(temperature);
        cardLayout->addWidget(humidity);
        m_values[QString("th%1_temp").arg(i + 1)] = temperature;
        m_values[QString("th%1_humi").arg(i + 1)] = humidity;
        metrics->addWidget(card, 0, i);
    }

    const QStringList otherFields = { "pt1_temp", "pt2_temp", "external_voltage" };
    const QStringList otherTitles = {
        QString::fromUtf8("PT100 温度 1"),
        QString::fromUtf8("PT100 温度 2"),
        QString::fromUtf8("外部电压")
    };
    for (int i = 0; i < otherFields.size(); ++i) {
        auto *card = makeCard(this);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(13, 10, 13, 10);
        auto *title = new QLabel(otherTitles.at(i), card);
        title->setObjectName("metricTitle");
        auto *value = new QLabel(i == 2 ? "--.- V" : "--.- ℃", card);
        value->setObjectName("metricValueSmall");
        cardLayout->addWidget(title);
        cardLayout->addWidget(value);
        m_values[otherFields.at(i)] = value;
        metrics->addWidget(card, 1, i);
    }
    for (int i = 0; i < 3; ++i)
        metrics->setColumnStretch(i, 1);
    layout->addLayout(metrics, 1);

    m_lastUpdate = new QLabel(QString::fromUtf8("最后更新：--"), this);
    m_lastUpdate->setObjectName("mutedText");
    layout->addWidget(m_lastUpdate);

    connect(m_deviceBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        refreshValues();
        emit currentDeviceChanged(currentDevice());
    });
    connect(m_manager, &DeviceManager::devicesChanged,
            this, &DeviceOverviewWidget::refreshDevices);
    connect(m_manager, &DeviceManager::deviceUpdated,
            this, &DeviceOverviewWidget::refreshDevice);
    refreshDevices();
}

DeviceProfile::DeviceKey DeviceOverviewWidget::currentDevice() const
{
    DeviceProfile::DeviceKey key;
    key.portIndex = m_deviceBox->currentData(Qt::UserRole).toInt();
    key.slaveId = m_deviceBox->currentData(Qt::UserRole + 1).toInt();
    return key;
}

DeviceState DeviceOverviewWidget::currentState() const
{
    return m_manager->device(currentDevice());
}

void DeviceOverviewWidget::selectDevice(const DeviceProfile::DeviceKey &key)
{
    for (int i = 0; i < m_deviceBox->count(); ++i) {
        if (m_deviceBox->itemData(i, Qt::UserRole).toInt() == key.portIndex
            && m_deviceBox->itemData(i, Qt::UserRole + 1).toInt() == key.slaveId) {
            m_deviceBox->setCurrentIndex(i);
            return;
        }
    }
}

void DeviceOverviewWidget::refreshDevices()
{
    const DeviceProfile::DeviceKey previous = currentDevice();
    const QList<DeviceState> devices = m_manager->allDevices();
    m_deviceTitle->setText(QString::fromUtf8("当前子板（已发现 %1 块）")
                               .arg(devices.size()));
    m_deviceBox->blockSignals(true);
    m_deviceBox->clear();
    for (const DeviceState &state : devices) {
        m_deviceBox->addItem(QString::fromUtf8("%1  ·  P%2 / ID %3")
                                 .arg(state.name)
                                 .arg(state.key.portIndex)
                                 .arg(state.key.slaveId));
        const int index = m_deviceBox->count() - 1;
        m_deviceBox->setItemData(index, state.key.portIndex, Qt::UserRole);
        m_deviceBox->setItemData(index, state.key.slaveId, Qt::UserRole + 1);
    }
    m_deviceBox->blockSignals(false);
    selectDevice(previous);
    if (m_deviceBox->currentIndex() < 0 && m_deviceBox->count() > 0)
        m_deviceBox->setCurrentIndex(0);
    refreshValues();
}

void DeviceOverviewWidget::refreshDevice(const DeviceProfile::DeviceKey &key)
{
    if (key == currentDevice())
        refreshValues();
}

void DeviceOverviewWidget::refreshValues()
{
    const DeviceProfile::DeviceKey key = currentDevice();
    const bool exists = m_manager->hasDevice(key);
    const DeviceState state = m_manager->device(key);
    m_linkState->setText(exists && state.online
        ? QString::fromUtf8("●  在线") : QString::fromUtf8("●  离线"));
    m_linkState->setProperty("online", exists && state.online);
    refreshDynamicStyle(m_linkState);

    for (auto it = m_values.begin(); it != m_values.end(); ++it) {
        it.value()->setText(state.values.contains(it.key())
            ? DeviceProfile::fieldDisplayValue(it.key(), state.values.value(it.key()))
            : (it.key() == "external_voltage" ? "--.- V" : "--.- ℃"));
        if (it.key().endsWith("_humi") && !state.values.contains(it.key()))
            it.value()->setText("--.- %RH");
    }
    m_lastUpdate->setText(state.lastUpdate.isValid()
        ? QString::fromUtf8("最后更新：%1").arg(state.lastUpdate.toString("yyyy-MM-dd  hh:mm:ss"))
        : QString::fromUtf8("最后更新：等待首次数据"));
}

// ============================================================
// ManualPanel
// ============================================================

ManualPanel::ManualPanel(DeviceManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    m_overview = new DeviceOverviewWidget(manager, this);
    layout->addWidget(m_overview, 7);

    auto *controlCard = makeCard(this);
    controlCard->setMinimumWidth(230);
    auto *controls = new QVBoxLayout(controlCard);
    controls->setContentsMargins(18, 18, 18, 18);
    controls->setSpacing(12);
    auto *title = new QLabel(QString::fromUtf8("手动输出"), controlCard);
    title->setObjectName("sectionTitle");
    controls->addWidget(title);
    auto *description = new QLabel(
        QString::fromUtf8("直接控制当前子板的加热回路"), controlCard);
    description->setObjectName("mutedText");
    description->setWordWrap(true);
    controls->addWidget(description);

    m_operationBanner = new QLabel(
        QString::fromUtf8("●  外部电压未超阈值\n可以操作"), controlCard);
    m_operationBanner->setObjectName("safeBanner");
    m_operationBanner->setMinimumHeight(54);
    m_operationBanner->setAlignment(Qt::AlignCenter);
    m_operationBanner->setWordWrap(true);
    controls->addWidget(m_operationBanner);

    m_ot3 = new QPushButton(QString::fromUtf8("OT3  回路一\n关闭"), controlCard);
    m_ot4 = new QPushButton(QString::fromUtf8("OT4  回路二\n关闭"), controlCard);
    for (QPushButton *button : { m_ot3, m_ot4 }) {
        button->setObjectName("outputButton");
        button->setCheckable(true);
        button->setMinimumHeight(82);
        controls->addWidget(button);
        connect(button, &QPushButton::clicked, this, &ManualPanel::toggleOutput);
    }

    controls->addStretch();
    layout->addWidget(controlCard, 3);

    connect(m_overview, &DeviceOverviewWidget::currentDeviceChanged,
            this, &ManualPanel::refreshControls);
    connect(m_manager, &DeviceManager::deviceUpdated,
            this, [this](const DeviceProfile::DeviceKey &key) {
        if (key == currentDevice())
            refreshControls();
    });
    refreshControls();
}

DeviceProfile::DeviceKey ManualPanel::currentDevice() const
{
    return m_overview->currentDevice();
}

void ManualPanel::setCurrentDevice(const DeviceProfile::DeviceKey &key)
{
    m_overview->selectDevice(key);
}

void ManualPanel::refreshControls()
{
    const DeviceState state = m_overview->currentState();
    const bool ready = state.online && state.values.contains("external_voltage");
    const bool locked = hasHighVoltage(state.values);
    m_operationBanner->setText(!ready
        ? QString::fromUtf8("●  等待子板完整数据\n暂不可操作")
        : locked
            ? QString::fromUtf8("⚠  外部电压超过阈值\n禁止操作 OT3 / OT4")
            : QString::fromUtf8("●  外部电压未超阈值\n可以操作"));
    m_operationBanner->setProperty("alarm", ready && locked);
    refreshDynamicStyle(m_operationBanner);
    const int ot3 = state.values.value("ot03").toInt();
    const int ot4 = state.values.value("ot04").toInt();
    const QList<QPair<QPushButton *, int>> buttons = { { m_ot3, ot3 }, { m_ot4, ot4 } };
    for (const auto &entry : buttons) {
        entry.first->blockSignals(true);
        entry.first->setChecked(entry.second != 0);
        entry.first->setEnabled(ready && !locked);
        entry.first->setProperty("outputOn", entry.second != 0);
        const QString output = entry.first == m_ot3 ? "OT3" : "OT4";
        const QString circuit = entry.first == m_ot3
            ? QString::fromUtf8("回路一") : QString::fromUtf8("回路二");
        entry.first->setText(QString::fromUtf8("%1  %2\n%3")
                                 .arg(output, circuit,
                                      entry.second ? QString::fromUtf8("已打开")
                                                   : QString::fromUtf8("已关闭")));
        entry.first->blockSignals(false);
        refreshDynamicStyle(entry.first);
    }
}

void ManualPanel::toggleOutput()
{
    const DeviceState state = m_overview->currentState();
    if (!state.online || !state.values.contains("external_voltage")
        || hasHighVoltage(state.values)) {
        refreshControls();
        return;
    }

    auto *button = qobject_cast<QPushButton *>(sender());
    if (!button)
        return;
    QMap<QString, QVariant> fields;
    fields[button == m_ot3 ? "ot03" : "ot04"] = button->isChecked() ? 1 : 0;
    emit writeRequested(currentDevice(), fields);
    button->setProperty("outputOn", button->isChecked());
    button->setText(QString::fromUtf8("%1  %2\n%3")
                        .arg(button == m_ot3 ? "OT3" : "OT4",
                             button == m_ot3 ? QString::fromUtf8("回路一")
                                             : QString::fromUtf8("回路二"),
                             button->isChecked() ? QString::fromUtf8("正在打开…")
                                                 : QString::fromUtf8("正在关闭…")));
    refreshDynamicStyle(button);
}

// ============================================================
// AutoPanel
// ============================================================

AutoPanel::AutoPanel(DeviceManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    m_overview = new DeviceOverviewWidget(manager, this);
    layout->addWidget(m_overview, 7);

    auto *runCard = makeCard(this);
    runCard->setMinimumWidth(245);
    auto *runLayout = new QVBoxLayout(runCard);
    runLayout->setContentsMargins(18, 18, 18, 18);
    runLayout->setSpacing(11);
    auto *title = new QLabel(QString::fromUtf8("自动温控"), runCard);
    title->setObjectName("sectionTitle");
    runLayout->addWidget(title);
    m_runState = new QLabel(QString::fromUtf8("●  程序未启动"), runCard);
    m_runState->setObjectName("runState");
    m_runState->setAlignment(Qt::AlignCenter);
    m_runState->setMinimumHeight(42);
    runLayout->addWidget(m_runState);

    auto *workTitle = new QLabel(QString::fromUtf8("当前工作状态"), runCard);
    workTitle->setObjectName("metricTitle");
    m_workState = new QLabel(QString::fromUtf8("已停止"), runCard);
    m_workState->setObjectName("statusPill");
    m_workState->setAlignment(Qt::AlignCenter);
    m_workState->setMinimumHeight(38);
    runLayout->addWidget(workTitle);
    runLayout->addWidget(m_workState);

    auto *averageTitle = new QLabel(QString::fromUtf8("三路平均温度"), runCard);
    averageTitle->setObjectName("metricTitle");
    m_averageTemp = new QLabel("--.- ℃", runCard);
    m_averageTemp->setObjectName("heroValue");
    m_averageTemp->setAlignment(Qt::AlignCenter);
    runLayout->addWidget(averageTitle);
    runLayout->addWidget(m_averageTemp);

    m_ruleText = new QLabel(runCard);
    m_ruleText->setObjectName("ruleText");
    m_ruleText->setWordWrap(true);
    runLayout->addWidget(m_ruleText);
    runLayout->addStretch();

    auto *buttons = new QHBoxLayout;
    m_start = new QPushButton(QString::fromUtf8("启  动"), runCard);
    m_stop = new QPushButton(QString::fromUtf8("停  止"), runCard);
    m_start->setObjectName("startButton");
    m_stop->setObjectName("dangerButton");
    m_start->setMinimumHeight(56);
    m_stop->setMinimumHeight(56);
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    runLayout->addLayout(buttons);
    layout->addWidget(runCard, 3);

    connect(m_start, &QPushButton::clicked, this, [this]() { emit runningChanged(true); });
    connect(m_stop, &QPushButton::clicked, this, [this]() { emit runningChanged(false); });
    connect(m_overview, &DeviceOverviewWidget::currentDeviceChanged,
            this, &AutoPanel::refreshState);
    connect(m_manager, &DeviceManager::deviceUpdated,
            this, [this](const DeviceProfile::DeviceKey &key) {
        if (key == m_overview->currentDevice())
            refreshState();
    });
    refreshParameters();
    refreshState();
}

void AutoPanel::setRunning(bool running)
{
    m_running = running;
    m_runState->setText(running
        ? QString::fromUtf8("●  自动运行中") : QString::fromUtf8("●  程序未启动"));
    m_runState->setProperty("running", running);
    m_start->setEnabled(!running);
    m_stop->setEnabled(running);
    refreshDynamicStyle(m_runState);
    refreshState();
}

void AutoPanel::refreshParameters()
{
    const AppConfig::GeneralConfig &config = AppConfig::instance().general();
    if (config.temperatureControlMode == "pid") {
        m_ruleText->setText(QString::fromUtf8(
            "PID 两级温控  │  目标 %1 ℃\n"
            "Kp %2  Ki %3  Kd %4\n"
            "输出 < %5%：关闭  │  %5～%6%：OT3  │  ≥ %6%：OT3+OT4\n"
            "最短切换间隔 %7 秒")
            .arg(config.temperatureTarget, 0, 'f', 1)
            .arg(config.pidKp, 0, 'f', 2)
            .arg(config.pidKi, 0, 'f', 2)
            .arg(config.pidKd, 0, 'f', 2)
            .arg(config.pidSingleStagePercent, 0, 'f', 0)
            .arg(config.pidDualStagePercent, 0, 'f', 0)
            .arg(config.relaySwitchIntervalSec));
    } else {
        m_ruleText->setText(QString::fromUtf8(
            "固定阈值模式  │  目标 %1 ℃\n低于 %2 ℃：两路打开\n"
            "高于 %3 ℃：两路关闭\n最短切换间隔 %4 秒")
            .arg(config.temperatureTarget, 0, 'f', 1)
            .arg(config.temperatureTarget - config.lowerHysteresis, 0, 'f', 1)
            .arg(config.temperatureTarget + config.upperHysteresis, 0, 'f', 1)
            .arg(config.relaySwitchIntervalSec));
    }
}

void AutoPanel::setCurrentDevice(const DeviceProfile::DeviceKey &key)
{
    m_overview->selectDevice(key);
}

void AutoPanel::refreshState()
{
    const DeviceState state = m_overview->currentState();
    if (!m_running) {
        m_workState->setText(QString::fromUtf8("已停止"));
    } else if (!state.online) {
        m_workState->setText(QString::fromUtf8("等待子板在线"));
    } else if (!state.values.contains("th1_temp")
               || !state.values.contains("th2_temp")
               || !state.values.contains("th3_temp")) {
        m_workState->setText(QString::fromUtf8("等待温度数据"));
    } else if (state.values.value("ot03").toInt()
               && state.values.value("ot04").toInt()) {
        m_workState->setText(QString::fromUtf8("两路输出"));
    } else if (state.values.value("ot03").toInt()) {
        m_workState->setText(QString::fromUtf8("回路一输出"));
    } else if (state.values.value("ot04").toInt()) {
        m_workState->setText(QString::fromUtf8("回路二输出"));
    } else {
        m_workState->setText(QString::fromUtf8("输出关闭"));
    }
    if (!state.values.contains("th1_temp")) {
        m_averageTemp->setText("--.- ℃");
        return;
    }
    const double average = averageField(
        state.values, { "th1_temp", "th2_temp", "th3_temp" });
    m_averageTemp->setText(QString::fromUtf8("%1 ℃").arg(average, 0, 'f', 1));
}

// ============================================================
// HistoryChart
// ============================================================

HistoryChart::HistoryChart(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::OpenHandCursor);
    setMouseTracking(true);
}

void HistoryChart::setRecords(const QVector<HistoryQuery::Record> &records)
{
    m_records = records;
    refreshAfterRecordsChanged();
}

void HistoryChart::appendRecord(const HistoryQuery::Record &record)
{
    m_records.append(record);
    refreshAfterRecordsChanged();
}

void HistoryChart::refreshAfterRecordsChanged()
{
    m_viewStart = m_followLatest ? maxViewStart()
                                 : qMin(m_viewStart, maxViewStart());
    m_crosshairRecordIndex = -1;
    emit viewStartChanged(m_viewStart);
    emit inspectionChanged(QString::fromUtf8("点击曲线查看数据，左右拖动切换时间段"));
    update();
}

int HistoryChart::visiblePointCount() const
{
    return qMin(m_records.size(), qMax(40, (width() - 106) / 4));
}

int HistoryChart::maxViewStart() const
{
    return qMax(0, m_records.size() - visiblePointCount());
}

void HistoryChart::followLatest()
{
    m_followLatest = true;
}

void HistoryChart::setViewStart(int start)
{
    const int bounded = qBound(0, start, maxViewStart());
    m_followLatest = bounded == maxViewStart();
    if (bounded == m_viewStart)
        return;
    m_viewStart = bounded;
    m_crosshairRecordIndex = -1;
    emit viewStartChanged(m_viewStart);
    emit inspectionChanged(QString::fromUtf8("点击曲线查看数据，左右拖动切换时间段"));
    update();
}

void HistoryChart::inspectAtX(int x)
{
    const int count = visiblePointCount();
    if (count <= 0)
        return;
    const qreal ratio = qBound(0.0,
        (x - 54.0) / qMax(1.0, width() - 106.0), 1.0);
    const int point = count == 1 ? 0
        : qRound(ratio * (count - 1));
    m_crosshairRecordIndex = m_viewStart + point;
    const HistoryQuery::Record &record = m_records.at(m_crosshairRecordIndex);
    const bool hasTemperature = record.values.contains("th1_temp")
        || record.values.contains("th2_temp") || record.values.contains("th3_temp");
    const bool hasHumidity = record.values.contains("th1_humi")
        || record.values.contains("th2_humi") || record.values.contains("th3_humi");
    const bool hasPt100 = record.values.contains("pt1_temp")
        || record.values.contains("pt2_temp");
    const QString temperature = hasTemperature
        ? QString::number(averageField(
              record.values, { "th1_temp", "th2_temp", "th3_temp" }), 'f', 1) + " ℃"
        : "--";
    const QString humidity = hasHumidity
        ? QString::number(averageField(
              record.values, { "th1_humi", "th2_humi", "th3_humi" }), 'f', 1) + " %RH"
        : "--";
    const QString pt100 = hasPt100
        ? QString::number(averageField(record.values, { "pt1_temp", "pt2_temp" }), 'f', 1) + " ℃"
        : "--";
    QString summary = QString::fromUtf8(
        "<b>%1</b>&nbsp;&nbsp; <span style='color:#c45f0a'>温湿度计 %2</span>"
        "&nbsp;&nbsp; <span style='color:#145fa8'>湿度 %3</span>"
        "&nbsp;&nbsp; <span style='color:#71368a'>PT100 %4</span>")
        .arg(record.timestamp.toString("yyyy-MM-dd hh:mm:ss"),
             temperature, humidity, pt100);
    if (record.values.contains("fleet_temp_min")) {
        summary += QString::fromUtf8(
            "&nbsp;&nbsp; 温度范围 %1~%2 ℃（%3 块）")
            .arg(record.values.value("fleet_temp_min").toInt() / 10.0, 0, 'f', 1)
            .arg(record.values.value("fleet_temp_max").toInt() / 10.0, 0, 'f', 1)
            .arg(record.values.value("fleet_count").toInt());
    }
    emit inspectionChanged(summary);
    update();
}

void HistoryChart::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragOrigin = event->pos();
        m_dragViewStart = m_viewStart;
        m_dragging = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void HistoryChart::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        if (qAbs(event->pos().x() - m_dragOrigin.x()) >= 6)
            m_dragging = true;
        const int pointDelta = static_cast<int>(
            static_cast<qreal>(m_dragOrigin.x() - event->pos().x())
            * visiblePointCount() / qMax(1, width() - 106));
        setViewStart(m_dragViewStart + pointDelta);
        event->accept();
        return;
    }
    inspectAtX(event->pos().x());
    QWidget::mouseMoveEvent(event);
}

void HistoryChart::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setCursor(Qt::OpenHandCursor);
        if (!m_dragging)
            inspectAtX(event->pos().x());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void HistoryChart::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), Qt::white);

    if (m_records.isEmpty()) {
        painter.setPen(QColor("#777777"));
        painter.drawText(rect(), Qt::AlignCenter, QString::fromUtf8("暂无历史数据"));
        return;
    }

    const int pointCount = visiblePointCount();
    auto recordAtPoint = [this](int point) -> const HistoryQuery::Record & {
        return m_records.at(m_viewStart + point);
    };
    bool hasTemperature = false;
    bool hasHumidity = false;
    double temperatureMin = 0.0;
    double temperatureMax = 0.0;
    double humidityMin = 0.0;
    double humidityMax = 0.0;

    for (int i = 0; i < pointCount; ++i) {
        const QMap<QString, QVariant> &values = recordAtPoint(i).values;
        if (values.contains("th1_temp") || values.contains("th2_temp")
            || values.contains("th3_temp")) {
            const double value = averageField(
                values, { "th1_temp", "th2_temp", "th3_temp" });
            temperatureMin = hasTemperature ? qMin(temperatureMin, value) : value;
            temperatureMax = hasTemperature ? qMax(temperatureMax, value) : value;
            hasTemperature = true;
        }
        if (values.contains("fleet_temp_min")) {
            const double low = values.value("fleet_temp_min").toInt() / 10.0;
            const double high = values.value("fleet_temp_max").toInt() / 10.0;
            temperatureMin = hasTemperature ? qMin(temperatureMin, low) : low;
            temperatureMax = hasTemperature ? qMax(temperatureMax, high) : high;
            hasTemperature = true;
        }
        if (values.contains("pt1_temp") || values.contains("pt2_temp")) {
            const double value = averageField(values, { "pt1_temp", "pt2_temp" });
            temperatureMin = hasTemperature ? qMin(temperatureMin, value) : value;
            temperatureMax = hasTemperature ? qMax(temperatureMax, value) : value;
            hasTemperature = true;
        }
        if (values.contains("th1_humi") || values.contains("th2_humi")
            || values.contains("th3_humi")) {
            const double value = averageField(
                values, { "th1_humi", "th2_humi", "th3_humi" });
            humidityMin = hasHumidity ? qMin(humidityMin, value) : value;
            humidityMax = hasHumidity ? qMax(humidityMax, value) : value;
            hasHumidity = true;
        }
    }

    if (!hasTemperature) {
        temperatureMin = 0.0;
        temperatureMax = 100.0;
    } else {
        const double padding = qMax(1.0, (temperatureMax - temperatureMin) * 0.15);
        temperatureMin -= padding;
        temperatureMax += padding;
    }
    if (!hasHumidity) {
        humidityMin = 0.0;
        humidityMax = 100.0;
    } else {
        const double padding = qMax(2.0, (humidityMax - humidityMin) * 0.15);
        humidityMin = qMax(0.0, humidityMin - padding);
        humidityMax = qMin(100.0, humidityMax + padding);
    }

    const int fontHeight = QFontMetrics(font()).height();
    const int timeAxisHeight = fontHeight * 2 + 10;
    const int legendHeight = fontHeight + 18;
    const QRectF plot = QRectF(rect()).adjusted(
        54, 18, -52, -(timeAxisHeight + legendHeight));
    const int axisTickCount = qBound(2, static_cast<int>(plot.height() / 32.0), 4);
    for (int i = 0; i <= axisTickCount; ++i) {
        const qreal y = plot.bottom() - plot.height() * i / axisTickCount;
        painter.setPen(QPen(QColor("#dddddd"), 1));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(QColor("#666666"));
        painter.drawText(QRectF(2, y - 9, 47, 18), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(temperatureMin
                             + (temperatureMax - temperatureMin) * i / axisTickCount, 'f', 1));
        painter.drawText(QRectF(plot.right() + 5, y - 9, 45, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(humidityMin
                             + (humidityMax - humidityMin) * i / axisTickCount, 'f', 0));
    }
    painter.setPen(QColor("#555555"));
    painter.drawText(QRectF(plot.left(), 0, 90, 18),
                     Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8("温度 ℃"));
    painter.drawText(QRectF(plot.right() - 90, 0, 90, 18),
                     Qt::AlignRight | Qt::AlignVCenter, QString::fromUtf8("湿度 %RH"));

    const int timeTickCount = qBound(2, static_cast<int>(plot.width() / 160.0), 12);
    for (int i = 0; i <= timeTickCount; ++i) {
        const int point = static_cast<int>(
            static_cast<qint64>(pointCount - 1) * i / timeTickCount);
        const qreal x = plot.left() + plot.width() * i / timeTickCount;
        painter.setPen(QPen(QColor("#aaaaaa"), 1));
        painter.drawLine(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + 4));
        painter.setPen(QColor("#666666"));
        painter.drawText(QRectF(x - 45, plot.bottom() + 5, 90, timeAxisHeight - 5),
                         Qt::AlignHCenter | Qt::AlignTop,
                         recordAtPoint(point).timestamp.toString("MM-dd\nhh:mm"));
    }

    QPainterPath temperaturePath;
    QPainterPath humidityPath;
    QPainterPath pt100Path;
    bool temperatureStarted = false;
    bool humidityStarted = false;
    bool pt100Started = false;
    QVector<QPointF> fleetRangeUpper;
    QVector<QPointF> fleetRangeLower;
    auto temperatureY = [&plot, temperatureMin, temperatureMax](double value) {
        return plot.bottom() - plot.height()
            * (value - temperatureMin) / (temperatureMax - temperatureMin);
    };
    auto humidityY = [&plot, humidityMin, humidityMax](double value) {
        return plot.bottom() - plot.height()
            * (value - humidityMin) / (humidityMax - humidityMin);
    };

    for (int i = 0; i < pointCount; ++i) {
        const HistoryQuery::Record &record = recordAtPoint(i);
        const qreal x = pointCount == 1 ? plot.left()
            : plot.left() + plot.width() * i / (pointCount - 1.0);
        if (record.values.contains("th1_temp") || record.values.contains("th2_temp")
            || record.values.contains("th3_temp")) {
            const qreal y = temperatureY(averageField(
                record.values, { "th1_temp", "th2_temp", "th3_temp" }));
            temperatureStarted ? temperaturePath.lineTo(x, y) : temperaturePath.moveTo(x, y);
            temperatureStarted = true;
        }
        if (record.values.contains("th1_humi") || record.values.contains("th2_humi")
            || record.values.contains("th3_humi")) {
            const qreal y = humidityY(averageField(
                record.values, { "th1_humi", "th2_humi", "th3_humi" }));
            humidityStarted ? humidityPath.lineTo(x, y) : humidityPath.moveTo(x, y);
            humidityStarted = true;
        }
        if (record.values.contains("pt1_temp") || record.values.contains("pt2_temp")) {
            const qreal y = temperatureY(averageField(
                record.values, { "pt1_temp", "pt2_temp" }));
            pt100Started ? pt100Path.lineTo(x, y) : pt100Path.moveTo(x, y);
            pt100Started = true;
        }
        if (record.values.contains("fleet_temp_min")) {
            fleetRangeUpper.append(QPointF(x, temperatureY(
                record.values.value("fleet_temp_max").toInt() / 10.0)));
            fleetRangeLower.append(QPointF(x, temperatureY(
                record.values.value("fleet_temp_min").toInt() / 10.0)));
        }
    }
    if (!fleetRangeUpper.isEmpty()) {
        QPainterPath rangePath;
        rangePath.moveTo(fleetRangeUpper.first());
        for (int i = 1; i < fleetRangeUpper.size(); ++i)
            rangePath.lineTo(fleetRangeUpper.at(i));
        for (int i = fleetRangeLower.size() - 1; i >= 0; --i)
            rangePath.lineTo(fleetRangeLower.at(i));
        rangePath.closeSubpath();
        painter.fillPath(rangePath, QColor(230, 126, 34, 45));
    }
    painter.setPen(QPen(QColor("#e67e22"), 2));
    painter.drawPath(temperaturePath);
    painter.setPen(QPen(QColor("#1976d2"), 2, Qt::DotLine));
    painter.drawPath(humidityPath);
    painter.setPen(QPen(QColor("#8e44ad"), 2, Qt::DashLine));
    painter.drawPath(pt100Path);

    if (m_crosshairRecordIndex >= m_viewStart
        && m_crosshairRecordIndex < m_viewStart + pointCount) {
        const int point = m_crosshairRecordIndex - m_viewStart;
        const qreal x = pointCount == 1 ? plot.left()
            : plot.left() + plot.width() * point / (pointCount - 1.0);
        const HistoryQuery::Record &record = m_records.at(m_crosshairRecordIndex);
        qreal crosshairY = plot.center().y();
        if (record.values.contains("th1_temp") || record.values.contains("th2_temp")
            || record.values.contains("th3_temp")) {
            crosshairY = temperatureY(averageField(
                record.values, { "th1_temp", "th2_temp", "th3_temp" }));
        } else if (record.values.contains("pt1_temp")
                   || record.values.contains("pt2_temp")) {
            crosshairY = temperatureY(averageField(
                record.values, { "pt1_temp", "pt2_temp" }));
        } else if (record.values.contains("th1_humi")
                   || record.values.contains("th2_humi")
                   || record.values.contains("th3_humi")) {
            crosshairY = humidityY(averageField(
                record.values, { "th1_humi", "th2_humi", "th3_humi" }));
        }
        painter.setPen(QPen(QColor("#555555"), 1, Qt::DashLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), crosshairY), QPointF(plot.right(), crosshairY));

        auto drawMarker = [&painter, x](const QColor &color, qreal y) {
            painter.setPen(QPen(color, 2));
            painter.setBrush(Qt::white);
            painter.drawEllipse(QPointF(x, y), 4, 4);
        };
        if (record.values.contains("th1_temp") || record.values.contains("th2_temp")
            || record.values.contains("th3_temp")) {
            drawMarker(QColor("#e67e22"), temperatureY(averageField(
                record.values, { "th1_temp", "th2_temp", "th3_temp" })));
        }
        if (record.values.contains("th1_humi") || record.values.contains("th2_humi")
            || record.values.contains("th3_humi")) {
            drawMarker(QColor("#1976d2"), humidityY(averageField(
                record.values, { "th1_humi", "th2_humi", "th3_humi" })));
        }
        if (record.values.contains("pt1_temp") || record.values.contains("pt2_temp")) {
            drawMarker(QColor("#8e44ad"), temperatureY(averageField(
                record.values, { "pt1_temp", "pt2_temp" })));
        }
        painter.setBrush(Qt::NoBrush);
    }

    const qreal legendTop = plot.bottom() + timeAxisHeight;
    const qreal legendY = legendTop + legendHeight / 2.0;
    const qreal legendColumnWidth = plot.width() / 3.0;
    painter.setPen(QPen(QColor("#e67e22"), 2));
    painter.drawLine(QPointF(plot.left(), legendY), QPointF(plot.left() + 20, legendY));
    painter.setPen(QColor("#c45f0a"));
    painter.drawText(QRectF(plot.left() + 25, legendTop, legendColumnWidth - 30, legendHeight),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     m_records.first().values.contains("fleet_count")
                         ? QString::fromUtf8("平均温度 / 范围")
                         : QString::fromUtf8("温湿度计温度"));
    painter.setPen(QPen(QColor("#1976d2"), 2, Qt::DotLine));
    painter.drawLine(QPointF(plot.left() + legendColumnWidth, legendY),
                     QPointF(plot.left() + legendColumnWidth + 20, legendY));
    painter.setPen(QColor("#145fa8"));
    painter.drawText(QRectF(plot.left() + legendColumnWidth + 25, legendTop,
                            legendColumnWidth - 30, legendHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8("温湿度计湿度"));
    painter.setPen(QPen(QColor("#8e44ad"), 2, Qt::DashLine));
    painter.drawLine(QPointF(plot.left() + legendColumnWidth * 2, legendY),
                     QPointF(plot.left() + legendColumnWidth * 2 + 20, legendY));
    painter.setPen(QColor("#71368a"));
    painter.drawText(QRectF(plot.left() + legendColumnWidth * 2 + 25, legendTop,
                            legendColumnWidth - 30, legendHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8("PT100 温度"));
}

// ============================================================
// HistoryWidget
// ============================================================

HistoryWidget::HistoryWidget(HistoryQuery *query,
                             DeviceManager *manager,
                             QWidget *parent)
    : QWidget(parent)
    , m_query(query)
    , m_manager(manager)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *filterCard = makeCard(this);
    auto *filters = new QHBoxLayout(filterCard);
    filters->setContentsMargins(14, 8, 14, 8);
    m_dateFrom = new QDateEdit(QDate::currentDate().addDays(-7), filterCard);
    m_dateTo = new QDateEdit(QDate::currentDate(), filterCard);
    for (QDateEdit *date : { m_dateFrom, m_dateTo }) {
        date->setCalendarPopup(true);
        date->setDisplayFormat("yyyy-MM-dd");
        date->setMinimumHeight(40);
    }
    m_deviceBox = new QComboBox(filterCard);
    configureDeviceCombo(m_deviceBox);
    m_deviceBox->setMinimumHeight(40);
    m_deviceBox->setMinimumWidth(170);
    auto *queryButton = new QPushButton(QString::fromUtf8("查  询"), filterCard);
    queryButton->setObjectName("primaryButton");
    queryButton->setMinimumSize(96, 40);
    auto *tableOnlyButton = new QPushButton(QString::fromUtf8("仅看表格"), filterCard);
    tableOnlyButton->setObjectName("secondaryButton");
    tableOnlyButton->setCheckable(true);
    tableOnlyButton->setMinimumSize(104, 40);
    filters->addWidget(new QLabel(QString::fromUtf8("日期"), filterCard));
    filters->addWidget(m_dateFrom);
    filters->addWidget(new QLabel("—", filterCard));
    filters->addWidget(m_dateTo);
    filters->addWidget(m_deviceBox);
    filters->addWidget(queryButton);
    filters->addWidget(tableOnlyButton);
    m_resultSummary = new QLabel(QString::fromUtf8("尚未查询"), filterCard);
    m_resultSummary->setObjectName("mutedText");
    filters->addStretch();
    filters->addWidget(m_resultSummary);
    layout->addWidget(filterCard);

    auto *chartCard = makeCard(this);
    auto *chartLayout = new QVBoxLayout(chartCard);
    chartLayout->setContentsMargins(10, 4, 10, 4);
    m_crosshairInfo = new QLabel(
        QString::fromUtf8("点击曲线查看数据，左右拖动切换时间段"), chartCard);
    m_crosshairInfo->setObjectName("crosshairInfo");
    m_crosshairInfo->setTextFormat(Qt::RichText);
    m_crosshairInfo->setMinimumHeight(36);
    m_crosshairInfo->setAlignment(Qt::AlignCenter);
    chartLayout->addWidget(m_crosshairInfo);
    m_chart = new HistoryChart(chartCard);
    chartLayout->addWidget(m_chart, 1);
    m_timeScroll = new QScrollBar(Qt::Horizontal, chartCard);
    m_timeScroll->setMinimumHeight(36);
    m_timeScroll->setSingleStep(10);
    chartLayout->addWidget(m_timeScroll);
    layout->addWidget(chartCard, 7);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({
        QString::fromUtf8("时间"), QString::fromUtf8("子板"),
        QString::fromUtf8("平均温度"), QString::fromUtf8("平均湿度"),
        QString::fromUtf8("高压状态")
    });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->setMinimumHeight(130);
    layout->addWidget(m_table, 4);
    m_table->setVisible(false);

    connect(queryButton, &QPushButton::clicked, this, &HistoryWidget::queryRecords);
    connect(tableOnlyButton, &QPushButton::toggled,
            this, [this, chartCard, tableOnlyButton](bool tableOnly) {
        chartCard->setVisible(!tableOnly);
        m_table->setVisible(tableOnly);
        tableOnlyButton->setText(tableOnly
            ? QString::fromUtf8("显示曲线") : QString::fromUtf8("仅看表格"));
    });
    connect(m_timeScroll, &QScrollBar::valueChanged,
            m_chart, &HistoryChart::setViewStart);
    connect(m_chart, &HistoryChart::viewStartChanged,
            m_timeScroll, &QScrollBar::setValue);
    connect(m_chart, &HistoryChart::inspectionChanged,
            m_crosshairInfo, &QLabel::setText);
    connect(m_manager, &DeviceManager::devicesChanged,
            this, &HistoryWidget::refreshDevices);
    refreshDevices();
}

void HistoryWidget::refreshDevices()
{
    const QString previous = m_deviceBox->currentData().toString();
    QMap<QPair<int, int>, QString> devices = m_historicalDevices;
    QSet<QPair<int, int>> discoveredDevices;
    for (const DeviceState &state : m_manager->allDevices()) {
        const QPair<int, int> key(state.key.portIndex, state.key.slaveId);
        devices.insert(key, state.name);
        discoveredDevices.insert(key);
    }

    m_deviceBox->blockSignals(true);
    m_deviceBox->clear();
    m_deviceBox->addItem(QString::fromUtf8("全部子板"), QString());
    for (auto it = devices.constBegin(); it != devices.constEnd(); ++it) {
        const QString name = it.value().isEmpty()
            ? QString("P%1-ID%2").arg(it.key().first).arg(it.key().second)
            : it.value();
        m_deviceBox->addItem(QString::fromUtf8("%1 (P%2 / ID %3)%4")
                                 .arg(name).arg(it.key().first).arg(it.key().second)
                                 .arg(discoveredDevices.contains(it.key())
                                          ? QString()
                                          : QString::fromUtf8(" · 历史")),
                             QString("%1:%2").arg(it.key().first).arg(it.key().second));
    }
    const int previousIndex = m_deviceBox->findData(previous);
    m_deviceBox->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
    m_deviceBox->blockSignals(false);
}

void HistoryWidget::reloadDevices()
{
    m_historicalDevices.clear();
    for (const HistoryQuery::DeviceInfo &info : m_query->availableDevices())
        m_historicalDevices.insert(
            QPair<int, int>(info.portIndex, info.slaveId), info.name);
    refreshDevices();
}

void HistoryWidget::onDataFilesChanged()
{
    reloadDevices();
    m_deviceIndexLoaded = true;
    m_loaded = false;
    loadRecords(true);
}

void HistoryWidget::activate()
{
    if (!m_deviceIndexLoaded) {
        reloadDevices();
        m_deviceIndexLoaded = true;
    } else {
        refreshDevices();
    }
    if (!m_loaded)
        loadRecords(true);
}

void HistoryWidget::queryRecords()
{
    const QString device = m_deviceBox->currentData().toString();
    if (m_loaded && m_loadedDateFrom == m_dateFrom->date()
        && m_loadedDateTo == m_dateTo->date() && m_loadedDevice == device) {
        m_chart->followLatest();
        m_chart->setRecords(m_chartRecords);
        updateTimeScroll();
        return;
    }
    loadRecords(true);
}

void HistoryWidget::onRecordAppended(const QDateTime &timestamp,
                                     const DeviceProfile::DeviceKey &key,
                                     const QString &deviceName,
                                     DeviceProfile::DeviceType type,
                                     const QMap<QString, QVariant> &values)
{
    const QPair<int, int> historyKey(key.portIndex, key.slaveId);
    if (!m_historicalDevices.contains(historyKey)) {
        m_historicalDevices.insert(historyKey, deviceName);
        refreshDevices();
    }
    if (!m_loaded || timestamp.date() < m_loadedDateFrom
        || timestamp.date() > m_loadedDateTo)
        return;

    const QString device = m_loadedDevice;
    if (!device.isEmpty()) {
        const QStringList parts = device.split(':');
        if (parts.size() != 2 || key.portIndex != parts.at(0).toInt()
            || key.slaveId != parts.at(1).toInt())
            return;
    }

    HistoryQuery::Record record;
    record.timestamp = timestamp;
    record.portIndex = key.portIndex;
    record.slaveId = key.slaveId;
    record.deviceName = deviceName;
    record.deviceType = type;
    record.values = values;
    ++m_rawRecordCount;
    if (device.isEmpty()) {
        mergeFleetRecord(m_chartRecords, record);
        m_chart->setRecords(m_chartRecords);
    } else {
        m_chartRecords.append(record);
        m_chart->appendRecord(record);
    }
    updateTimeScroll();
    m_resultSummary->setText(device.isEmpty()
        ? QString::fromUtf8("共 %1 条明细 / %2 个时间点")
              .arg(m_rawRecordCount).arg(m_chart->recordCount())
        : QString::fromUtf8("共 %1 条记录").arg(m_rawRecordCount));

    if (m_table->rowCount() >= 500)
        m_table->removeRow(m_table->rowCount() - 1);
    m_table->insertRow(0);
    setTableRow(0, record);
}

void HistoryWidget::loadRecords(bool resetToLatest)
{
    HistoryQuery::Filter filter;
    filter.dateFrom = m_dateFrom->date();
    filter.dateTo = m_dateTo->date();
    const QString device = m_deviceBox->currentData().toString();
    if (!device.isEmpty()) {
        const QStringList parts = device.split(':');
        if (parts.size() == 2) {
            filter.portIndex = parts.at(0).toInt();
            filter.slaveId = parts.at(1).toInt();
        }
    }

    const QVector<HistoryQuery::Record> records = m_query->query(filter);
    m_loaded = true;
    m_loadedDateFrom = m_dateFrom->date();
    m_loadedDateTo = m_dateTo->date();
    m_loadedDevice = device;
    m_rawRecordCount = records.size();
    m_chartRecords.clear();
    if (device.isEmpty()) {
        for (const HistoryQuery::Record &record : records)
            mergeFleetRecord(m_chartRecords, record);
    } else {
        m_chartRecords = records;
    }
    if (resetToLatest)
        m_chart->followLatest();
    {
        const QSignalBlocker blocker(m_timeScroll);
        m_chart->setRecords(m_chartRecords);
        updateTimeScroll();
    }
    m_resultSummary->setText(device.isEmpty()
        ? QString::fromUtf8("共 %1 条明细 / %2 个时间点")
              .arg(records.size()).arg(m_chartRecords.size())
        : QString::fromUtf8("共 %1 条记录").arg(records.size()));

    const int visibleRows = qMin(records.size(), 500);
    m_table->setRowCount(visibleRows);
    for (int row = 0; row < visibleRows; ++row)
        setTableRow(row, records.at(records.size() - 1 - row));
}

void HistoryWidget::updateTimeScroll()
{
    const QSignalBlocker blocker(m_timeScroll);
    m_timeScroll->setRange(0, m_chart->maxViewStart());
    m_timeScroll->setPageStep(m_chart->visiblePointCount());
    m_timeScroll->setValue(m_chart->viewStart());
}

void HistoryWidget::setTableRow(int row, const HistoryQuery::Record &record)
{
    const double temperature = averageField(
        record.values, { "th1_temp", "th2_temp", "th3_temp" });
    const double humidity = averageField(
        record.values, { "th1_humi", "th2_humi", "th3_humi" });
    m_table->setItem(row, 0, new QTableWidgetItem(
        record.timestamp.toString("MM-dd hh:mm:ss")));
    m_table->setItem(row, 1, new QTableWidgetItem(record.deviceName));
    m_table->setItem(row, 2, new QTableWidgetItem(
        QString::number(temperature, 'f', 1) + " ℃"));
    m_table->setItem(row, 3, new QTableWidgetItem(
        QString::number(humidity, 'f', 1) + " %RH"));
    const bool highVoltage = hasHighVoltage(record.values);
    auto *highVoltageItem = new QTableWidgetItem(highVoltage
        ? QString::fromUtf8("带电") : QString::fromUtf8("正常"));
    highVoltageItem->setData(Qt::ForegroundRole,
                             highVoltage ? QColor("#c93632") : QColor("#168f4f"));
    m_table->setItem(row, 4, highVoltageItem);
}

// ============================================================
// SettingsWidget
// ============================================================

SettingsWidget::SettingsWidget(StorageRotator *rotator, QWidget *parent)
    : QWidget(parent)
    , m_rotator(rotator)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->verticalScrollBar()->setSingleStep(44);
    scrollArea->verticalScrollBar()->setStyleSheet(QString::fromUtf8(
        "QScrollBar:vertical { width: 32px; background: #eeeeee; }"
        "QScrollBar::handle:vertical { background: #666666; min-height: 72px; margin: 2px 4px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));
    auto *content = new QWidget(scrollArea);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->setAlignment(Qt::AlignTop);
    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea);

    auto *temperatureCard = makeCard(this);
    auto *temperatureLayout = new QVBoxLayout(temperatureCard);
    temperatureLayout->setContentsMargins(16, 10, 16, 10);
    temperatureLayout->setSpacing(8);
    auto *temperatureTitleRow = new QHBoxLayout;
    auto *temperatureTitle = new QLabel(QString::fromUtf8("温度控制"), temperatureCard);
    temperatureTitle->setObjectName("sectionTitle");
    temperatureTitleRow->addWidget(temperatureTitle);
    temperatureTitleRow->addStretch();
    temperatureTitleRow->addWidget(new QLabel(QString::fromUtf8("控制模式"), temperatureCard));
    m_controlMode = new QComboBox(temperatureCard);
    m_controlMode->addItem(QString::fromUtf8("固定阈值"), "threshold");
    m_controlMode->addItem(QString::fromUtf8("PID 两级输出"), "pid");
    temperatureTitleRow->addWidget(m_controlMode);
    temperatureLayout->addLayout(temperatureTitleRow);
    auto *parameterGrid = new QGridLayout;
    parameterGrid->setHorizontalSpacing(18);
    const QStringList labels = {
        QString::fromUtf8("目标温度 XX.X"),
        QString::fromUtf8("下限回差 YY.Y"),
        QString::fromUtf8("上限回差 ZZ.Z"),
        QString::fromUtf8("外部高压触发阈值"),
        QString::fromUtf8("PID 比例 Kp"),
        QString::fromUtf8("PID 积分 Ki"),
        QString::fromUtf8("PID 微分 Kd")
    };
    const AppConfig::GeneralConfig &config = AppConfig::instance().general();
    const QList<double> values = {
        config.temperatureTarget, config.lowerHysteresis,
        config.upperHysteresis, config.highVoltageThreshold,
        config.pidKp, config.pidKi, config.pidKd
    };
    QList<QDoubleSpinBox *> inputs;
    for (int i = 0; i < labels.size(); ++i) {
        auto *label = new QLabel(labels.at(i), temperatureCard);
        label->setObjectName("metricTitle");
        auto *input = new QDoubleSpinBox(temperatureCard);
        if (i == 0) {
            input->setRange(-20.0, 80.0);
            input->setSuffix(" ℃");
            input->setSingleStep(0.5);
        } else if (i < 3) {
            input->setRange(0.1, 30.0);
            input->setSuffix(" ℃");
            input->setSingleStep(0.5);
        } else if (i == 3) {
            input->setRange(0.0, 100.0);
            input->setSuffix(" V");
            input->setSingleStep(0.1);
        } else if (i == 5) {
            input->setRange(0.0, 10.0);
            input->setSingleStep(0.05);
        } else {
            input->setRange(0.0, 100.0);
            input->setSingleStep(0.5);
        }
        input->setDecimals(i >= 4 ? 2 : 1);
        input->setValue(values.at(i));
        input->setButtonSymbols(QAbstractSpinBox::NoButtons);
        input->setAlignment(Qt::AlignCenter);
        input->setMinimumHeight(46);
        auto *decrease = new QPushButton(QString::fromUtf8("−"), temperatureCard);
        auto *increase = new QPushButton("+", temperatureCard);
        for (QPushButton *button : { decrease, increase }) {
            button->setObjectName("adjustButton");
            button->setMinimumSize(42, 46);
            button->setAutoRepeat(true);
            button->setAutoRepeatDelay(400);
            button->setAutoRepeatInterval(120);
        }
        auto *adjustment = new QHBoxLayout;
        adjustment->setSpacing(0);
        adjustment->addWidget(decrease);
        adjustment->addWidget(input, 1);
        adjustment->addWidget(increase);
        const int column = i % 4;
        const int row = (i / 4) * 2;
        parameterGrid->addWidget(label, row, column);
        parameterGrid->addLayout(adjustment, row + 1, column);
        parameterGrid->setColumnStretch(column, 1);
        inputs.append(input);
        connect(decrease, &QPushButton::clicked, input, &QDoubleSpinBox::stepDown);
        connect(increase, &QPushButton::clicked, input, &QDoubleSpinBox::stepUp);
    }
    m_targetTemp = inputs.at(0);
    m_lowerHysteresis = inputs.at(1);
    m_upperHysteresis = inputs.at(2);
    m_highVoltageThreshold = inputs.at(3);
    m_pidKp = inputs.at(4);
    m_pidKi = inputs.at(5);
    m_pidKd = inputs.at(6);
    const int modeIndex = m_controlMode->findData(config.temperatureControlMode);
    m_controlMode->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
    temperatureLayout->addLayout(parameterGrid);

    m_formula = new QLabel(temperatureCard);
    m_formula->setObjectName("formulaBox");
    m_formula->setWordWrap(true);
    temperatureLayout->addWidget(m_formula);
    layout->addWidget(temperatureCard);

    auto makeIntegerAdjustment = [](QSpinBox *input, QWidget *parent) {
        input->setButtonSymbols(QAbstractSpinBox::NoButtons);
        input->setAlignment(Qt::AlignCenter);
        input->setMinimumSize(120, 44);
        auto *decrease = new QPushButton(QString::fromUtf8("−"), parent);
        auto *increase = new QPushButton("+", parent);
        for (QPushButton *button : { decrease, increase }) {
            button->setObjectName("adjustButton");
            button->setMinimumSize(48, 44);
            button->setAutoRepeat(true);
            button->setAutoRepeatDelay(400);
            button->setAutoRepeatInterval(120);
        }
        connect(decrease, &QPushButton::clicked, input, &QSpinBox::stepDown);
        connect(increase, &QPushButton::clicked, input, &QSpinBox::stepUp);
        auto *adjustment = new QHBoxLayout;
        adjustment->setSpacing(0);
        adjustment->addWidget(decrease);
        adjustment->addWidget(input);
        adjustment->addWidget(increase);
        return adjustment;
    };

    auto *storageCard = makeCard(this);
    auto *storageLayout = new QVBoxLayout(storageCard);
    storageLayout->setContentsMargins(16, 10, 16, 10);
    storageLayout->setSpacing(7);
    auto *storageTop = new QHBoxLayout;
    auto *storageText = new QVBoxLayout;
    auto *storageTitle = new QLabel(QString::fromUtf8("数据文件与存储空间"), storageCard);
    storageTitle->setObjectName("sectionTitle");
    storageText->addWidget(storageTitle);
    auto *storageHint = new QLabel(
        QString::fromUtf8("自动记录完整测量/IO 信息，可按日期清理历史 CSV"), storageCard);
    storageHint->setObjectName("mutedText");
    storageText->addWidget(storageHint);
    storageTop->addLayout(storageText, 1);
    storageTop->addWidget(new QLabel(QString::fromUtf8("记录周期"), storageCard));
    m_recordInterval = new QSpinBox(storageCard);
    m_recordInterval->setRange(1, 3600);
    m_recordInterval->setSuffix(QString::fromUtf8(" 秒"));
    m_recordInterval->setValue(config.recordIntervalSec);
    storageTop->addLayout(makeIntegerAdjustment(m_recordInterval, storageCard));
    storageLayout->addLayout(storageTop);

    auto *storageActions = new QHBoxLayout;
    m_storageSummary = new QLabel(QString::fromUtf8("正在读取存储空间…"), storageCard);
    m_storageSummary->setObjectName("mutedText");
    m_storageSummary->setWordWrap(true);
    storageActions->addWidget(m_storageSummary, 1);
    storageActions->addWidget(new QLabel(QString::fromUtf8("删除此日期之前"), storageCard));
    m_deleteBeforeDate = new QDateEdit(QDate::currentDate(), storageCard);
    m_deleteBeforeDate->setCalendarPopup(true);
    m_deleteBeforeDate->setDisplayFormat("yyyy-MM-dd");
    m_deleteBeforeDate->setMaximumDate(QDate::currentDate());
    m_deleteBeforeDate->setMinimumWidth(132);
    storageActions->addWidget(m_deleteBeforeDate);
    auto *refreshStorage = new QPushButton(QString::fromUtf8("刷新"), storageCard);
    refreshStorage->setObjectName("secondaryButton");
    refreshStorage->setMinimumHeight(42);
    storageActions->addWidget(refreshStorage);
    auto *deleteOld = new QPushButton(QString::fromUtf8("删除旧数据"), storageCard);
    deleteOld->setObjectName("dangerButton");
    deleteOld->setMinimumHeight(42);
    storageActions->addWidget(deleteOld);
    storageLayout->addLayout(storageActions);
    layout->addWidget(storageCard);

    auto *relayCard = makeCard(this);
    auto *relayLayout = new QHBoxLayout(relayCard);
    relayLayout->setContentsMargins(16, 10, 16, 10);
    auto *relayText = new QVBoxLayout;
    auto *relayTitle = new QLabel(QString::fromUtf8("继电器切换保护"), relayCard);
    relayTitle->setObjectName("sectionTitle");
    relayText->addWidget(relayTitle);
    auto *relayHint = new QLabel(
        QString::fromUtf8("自动温控 OT3/OT4 两次状态改变的最短间隔"), relayCard);
    relayHint->setObjectName("mutedText");
    relayText->addWidget(relayHint);
    relayLayout->addLayout(relayText, 1);
    m_relaySwitchInterval = new QSpinBox(relayCard);
    m_relaySwitchInterval->setRange(1, 3600);
    m_relaySwitchInterval->setSuffix(QString::fromUtf8(" 秒"));
    m_relaySwitchInterval->setValue(config.relaySwitchIntervalSec);
    relayLayout->addLayout(makeIntegerAdjustment(m_relaySwitchInterval, relayCard));
    layout->addWidget(relayCard);

    auto *buttonRow = new QHBoxLayout;
    auto *rescan = new QPushButton(QString::fromUtf8("重新扫描子板"), this);
    rescan->setObjectName("secondaryButton");
    rescan->setMinimumSize(180, 48);
    m_actionFeedback = new QLabel(this);
    m_actionFeedback->setObjectName("actionFeedback");
    m_actionFeedback->setAlignment(Qt::AlignCenter);
    m_actionFeedback->setMinimumHeight(40);
    m_actionFeedback->setWordWrap(true);
    m_feedbackTimer = new QTimer(this);
    m_feedbackTimer->setSingleShot(true);
    connect(m_feedbackTimer, &QTimer::timeout, this, [this]() {
        m_actionFeedback->clear();
    });
    auto *save = new QPushButton(QString::fromUtf8("保存并应用"), this);
    save->setObjectName("primaryButton");
    save->setMinimumSize(180, 48);
    buttonRow->addWidget(rescan);
    buttonRow->addWidget(m_actionFeedback, 1);
    buttonRow->addWidget(save);
    layout->addLayout(buttonRow);

    auto updateFormula = [this]() {
        const double target = m_targetTemp->value();
        const bool pid = m_controlMode->currentData().toString() == "pid";
        m_lowerHysteresis->setEnabled(!pid);
        m_upperHysteresis->setEnabled(!pid);
        m_pidKp->setEnabled(pid);
        m_pidKi->setEnabled(pid);
        m_pidKd->setEnabled(pid);
        if (pid) {
            m_formula->setText(QString::fromUtf8(
                "PID 需求量：< 10% 全关，10～60% 开 OT3，≥ 60% 开 OT3+OT4；"
                "Kp=%1、Ki=%2、Kd=%3。外部电压 > %4 V 时强制切断。")
                .arg(m_pidKp->value(), 0, 'f', 2)
                .arg(m_pidKi->value(), 0, 'f', 2)
                .arg(m_pidKd->value(), 0, 'f', 2)
                .arg(m_highVoltageThreshold->value(), 0, 'f', 1));
        } else {
            m_formula->setText(QString::fromUtf8(
                "T < %1 ℃：OT3、OT4 打开    │    T > %2 ℃：OT3 打开    │    "
                "T > %3 ℃：全部关闭    │    外部电压 > %4 V：高压告警")
                .arg(target - m_lowerHysteresis->value(), 0, 'f', 1)
                .arg(target, 0, 'f', 1)
                .arg(target + m_upperHysteresis->value(), 0, 'f', 1)
                .arg(m_highVoltageThreshold->value(), 0, 'f', 1));
        }
    };
    connect(m_controlMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [updateFormula](int) { updateFormula(); });
    connect(m_targetTemp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [updateFormula](double) { updateFormula(); });
    connect(m_lowerHysteresis, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [updateFormula](double) { updateFormula(); });
    connect(m_upperHysteresis, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [updateFormula](double) { updateFormula(); });
    connect(m_highVoltageThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [updateFormula](double) { updateFormula(); });
    connect(m_pidKp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [updateFormula](double) { updateFormula(); });
    connect(m_pidKi, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [updateFormula](double) { updateFormula(); });
    connect(m_pidKd, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [updateFormula](double) { updateFormula(); });
    auto clearFeedback = [this]() {
        m_feedbackTimer->stop();
        m_actionFeedback->clear();
    };
    for (QDoubleSpinBox *input : {
             m_targetTemp, m_lowerHysteresis,
             m_upperHysteresis, m_highVoltageThreshold,
             m_pidKp, m_pidKi, m_pidKd }) {
        connect(input, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [clearFeedback](double) { clearFeedback(); });
    }
    for (QSpinBox *input : { m_recordInterval, m_relaySwitchInterval }) {
        connect(input, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [clearFeedback](int) { clearFeedback(); });
    }
    connect(save, &QPushButton::clicked, this, &SettingsWidget::saveSettings);
    connect(rescan, &QPushButton::clicked, this, &SettingsWidget::rescanRequested);
    connect(refreshStorage, &QPushButton::clicked,
            this, &SettingsWidget::refreshStorageInfo);
    connect(deleteOld, &QPushButton::clicked, this, &SettingsWidget::deleteOldData);
    auto *storageTimer = new QTimer(this);
    connect(storageTimer, &QTimer::timeout, this, &SettingsWidget::refreshStorageInfo);
    storageTimer->start(10000);
    updateFormula();
    refreshStorageInfo();
}

void SettingsWidget::saveSettings()
{
    AppConfig &config = AppConfig::instance();
    config.general().temperatureTarget = m_targetTemp->value();
    config.general().temperatureControlMode = m_controlMode->currentData().toString();
    config.general().lowerHysteresis = m_lowerHysteresis->value();
    config.general().upperHysteresis = m_upperHysteresis->value();
    config.general().pidKp = m_pidKp->value();
    config.general().pidKi = m_pidKi->value();
    config.general().pidKd = m_pidKd->value();
    config.general().highVoltageThreshold = m_highVoltageThreshold->value();
    config.general().relaySwitchIntervalSec = m_relaySwitchInterval->value();
    config.general().recordIntervalSec = m_recordInterval->value();
    const bool saved = config.configFilePath().isEmpty()
        || config.save(config.configFilePath());
    showActionFeedback(saved
        ? QString::fromUtf8("✓  配置已保存并应用")
        : QString::fromUtf8("⚠  配置保存失败"), saved);
    emit settingsSaved();
}

void SettingsWidget::refreshStorageInfo()
{
    if (!m_rotator || !m_storageSummary)
        return;
    const StorageRotator::StorageStatus status = m_rotator->storageStatus();
    if (!status.ready) {
        m_storageSummary->setText(QString::fromUtf8("存储不可用：%1").arg(status.error));
        return;
    }
    const QString range = status.oldestDate.isValid()
        ? QString::fromUtf8("%1～%2")
              .arg(status.oldestDate.toString("yyyy-MM-dd"))
              .arg(status.newestDate.toString("yyyy-MM-dd"))
        : QString::fromUtf8("暂无记录");
    m_storageSummary->setText(QString::fromUtf8(
        "剩余 %1 / 总计 %2  │  日志 %3（%4 个文件，%5）")
        .arg(formatBytes(status.bytesAvailable))
        .arg(formatBytes(status.bytesTotal))
        .arg(formatBytes(status.logBytes))
        .arg(status.fileCount)
        .arg(range));
}

void SettingsWidget::deleteOldData()
{
    if (!m_rotator || !m_deleteBeforeDate)
        return;
    const QDate cutoff = m_deleteBeforeDate->date();
    const StorageRotator::DeleteResult preview =
        m_rotator->previewDeleteBefore(cutoff);
    if (!preview.valid) {
        showActionFeedback(QString::fromUtf8("⚠  %1").arg(preview.error), false);
        return;
    }
    if (preview.files == 0) {
        showActionFeedback(QString::fromUtf8("没有 %1 之前的数据")
                               .arg(cutoff.toString("yyyy-MM-dd")), true);
        return;
    }
    const QString question = QString::fromUtf8(
        "将永久删除 %1 之前的 %2 个 CSV 文件，共 %3。\n"
        "当天及之后的数据不会删除。是否继续？")
        .arg(cutoff.toString("yyyy-MM-dd"))
        .arg(preview.files)
        .arg(formatBytes(preview.bytes));
    if (QMessageBox::warning(this, QString::fromUtf8("确认删除历史数据"),
                             question, QMessageBox::Yes | QMessageBox::No,
                             QMessageBox::No) != QMessageBox::Yes)
        return;

    const StorageRotator::DeleteResult removed = m_rotator->deleteBefore(cutoff);
    refreshStorageInfo();
    if (removed.files > 0)
        emit dataFilesChanged();
    showActionFeedback(removed.valid
        ? QString::fromUtf8("✓  已删除 %1 个文件，释放 %2")
              .arg(removed.files).arg(formatBytes(removed.bytes))
        : QString::fromUtf8("⚠  %1").arg(removed.error), removed.valid);
}

void SettingsWidget::showActionFeedback(const QString &message, bool success)
{
    m_actionFeedback->setText(message);
    m_actionFeedback->setProperty("success", success);
    refreshDynamicStyle(m_actionFeedback);
    m_feedbackTimer->start(3000);
}

// ============================================================
// MainWindow
// ============================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_startedAt = QDateTime::currentDateTime();
    setWindowTitle(QString::fromUtf8("智能环境控制系统"));
    resize(1024, 600);
    setMinimumSize(800, 480);
    setupUi();
    startServices();
    switchPage(0);
}

MainWindow::~MainWindow()
{
    if (m_scheduler)
        m_scheduler->stop();
}

void MainWindow::setupUi()
{
    qApp->setStyleSheet(R"(
        * { font-family: "Noto Sans CJK SC", "Microsoft YaHei", sans-serif; }
        QMainWindow, QWidget#appRoot { background: white; color: #202020; }
        QFrame#sideBar { background: #202020; border: none; }
        QLabel#brandTitle { color: white; font-size: 19px; font-weight: 700; }
        QPushButton#navButton { color: #dddddd; background: transparent; border: none;
            border-radius: 0; text-align: left; padding-left: 22px; font-size: 16px; }
        QPushButton#navButton:hover { background: #333333; color: white; }
        QPushButton#navButton:checked { background: #555555; color: white; border-left: 4px solid white;
            padding-left: 18px; font-weight: 700; }
        QFrame#topBar { background: white; border-bottom: 1px solid #cccccc; }
        QLabel#pageTitle { color: #202020; font-size: 21px; font-weight: 700; }
        QLabel#clock { color: #666666; font-size: 13px; }
        QLabel#systemPill { background: #e8f7ee; color: #168f4f; border: 1px solid #7bc99d;
            border-radius: 0; padding: 5px 12px; font-weight: 700; }
        QLabel#systemPill[alarm="true"] { background: #fdeceb; color: #c93632; border-color: #e5a3a0; }
        QLabel#systemPill[neutral="true"] { background: #eeeeee; color: #555555; border-color: #bbbbbb; }
        QFrame#card { background: white; border: 1px solid #cccccc; border-radius: 0; }
        QLabel#sectionTitle { color: #202020; font-size: 17px; font-weight: 700; }
        QLabel#metricTitle { color: #666666; font-size: 12px; }
        QLabel#metricValue { color: #202020; font-size: 23px; font-weight: 700; }
        QLabel#metricSubValue { color: #555555; font-size: 15px; font-weight: 600; }
        QLabel#metricValueSmall { color: #202020; font-size: 20px; font-weight: 700; }
        QLabel#heroValue { color: #202020; font-size: 34px; font-weight: 700; }
        QLabel#mutedText { color: #777777; font-size: 12px; }
        QLabel#noticeText { color: #333333; background: #f3f3f3; border: 1px solid #cccccc;
            border-radius: 0; padding: 9px; }
        QLabel#ruleText { color: #333333; background: #f3f3f3; border: 1px solid #cccccc;
            border-radius: 0; padding: 10px; line-height: 1.5; }
        QLabel#formulaBox { color: #333333; background: #f3f3f3; border: 1px solid #cccccc;
            border-radius: 0; padding: 11px; }
        QLabel#actionFeedback { color: #c93632; font-size: 13px; font-weight: 700; padding: 3px 8px; }
        QLabel#actionFeedback[success="true"] { color: #168f4f; }
        QLabel#crosshairInfo { color: #333333; background: #f7f7f7; border: 1px solid #cccccc;
            padding: 6px 10px; font-size: 13px; }
        QLabel#safeBanner { color: #168f4f; background: #e8f7ee; border: 1px solid #7bc99d;
            border-radius: 0; font-weight: 600; }
        QLabel#safeBanner[alarm="true"] { color: white; background: #c93632; border-color: #ad2825; }
        QLabel#statusPill { color: #c93632; background: #fdeceb; border: 1px solid #e5a3a0;
            border-radius: 0; padding: 4px 11px; }
        QLabel#statusPill[online="true"] { color: #168f4f; background: #e8f7ee; border-color: #7bc99d; }
        QLabel#runState { color: #555555; background: #eeeeee; border: 1px solid #cccccc;
            border-radius: 0; font-weight: 700; }
        QLabel#runState[running="true"] { color: white; background: #168f4f; border-color: #107b43; }
        QComboBox, QDateEdit, QSpinBox, QDoubleSpinBox { background: white; border: 1px solid #aaaaaa;
            border-radius: 0; padding: 6px 10px; font-size: 14px; }
        QComboBox:focus { border: 2px solid #1976d2; }
        QComboBox::drop-down { border: none; width: 28px; }
        QComboBox QAbstractItemView { background: white; color: #202020; border: 1px solid #777777;
            outline: 0; selection-background-color: #1976d2; selection-color: white; }
        QComboBox QAbstractItemView::item { min-height: 44px; padding-left: 10px; }
        QPushButton { border-radius: 0; font-size: 14px; }
        QPushButton#primaryButton { color: white; background: #333333; border: 1px solid #222222; font-weight: 700; }
        QPushButton#primaryButton:hover { background: #111111; }
        QPushButton#primaryButton:disabled { color: #777777; background: #dddddd; border-color: #cccccc; }
        QPushButton#secondaryButton { color: #333333; background: white; border: 1px solid #777777;
            font-weight: 700; }
        QPushButton#secondaryButton:hover { background: #eeeeee; }
        QPushButton#secondaryButton:checked { color: white; background: #555555; border-color: #333333; }
        QPushButton#startButton { color: white; background: #168f4f; border: 1px solid #107b43; font-weight: 700; }
        QPushButton#startButton:hover { background: #107b43; }
        QPushButton#startButton:disabled { color: #777777; background: #dddddd; border-color: #cccccc; }
        QPushButton#dangerButton { color: white; background: #c93632; border: 1px solid #ad2825; font-weight: 700; }
        QPushButton#dangerButton:disabled { color: #999999; background: #eeeeee; border-color: #cccccc; }
        QPushButton#outputButton { color: #333333; background: #eeeeee; border: 1px solid #bbbbbb; font-weight: 700; }
        QPushButton#outputButton[outputOn="true"] { color: white; background: #168f4f; border-color: #107b43; }
        QPushButton#outputButton:disabled { color: #c93632; background: #fdeceb; border-color: #e5a3a0; }
        QPushButton#adjustButton { color: #222222; background: #f1f1f1; border: 1px solid #aaaaaa;
            font-size: 24px; font-weight: 700; }
        QPushButton#adjustButton:pressed { color: white; background: #333333; }
        QScrollBar:horizontal { height: 36px; background: #eeeeee; border: 1px solid #cccccc; margin: 0; }
        QScrollBar::handle:horizontal { background: #666666; min-width: 100px; margin: 4px 2px; }
        QScrollBar::handle:horizontal:hover { background: #333333; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; border: none; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: #eeeeee; }
        QTableWidget { background: white; alternate-background-color: #f5f5f5; border: 1px solid #cccccc;
            border-radius: 0; gridline-color: #dddddd; font-size: 12px; }
        QHeaderView::section { color: #333333; background: #e8e8e8; border: none; padding: 6px; font-weight: 700; }
        QLabel#bottomStatus { color: #555555; background: white; border-top: 1px solid #cccccc; padding-left: 12px; }
    )");

    auto *root = new QWidget(this);
    root->setObjectName("appRoot");
    auto *rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *sideBar = new QFrame(root);
    sideBar->setObjectName("sideBar");
    sideBar->setFixedWidth(182);
    auto *sideLayout = new QVBoxLayout(sideBar);
    sideLayout->setContentsMargins(14, 22, 14, 18);
    sideLayout->setSpacing(9);
    auto *brandTitle = new QLabel(QString::fromUtf8("环境控制系统"), sideBar);
    brandTitle->setObjectName("brandTitle");
    sideLayout->addWidget(brandTitle);
    sideLayout->addSpacing(28);

    const QStringList navTexts = {
        QString::fromUtf8("手动操作"), QString::fromUtf8("自动运行"),
        QString::fromUtf8("参数设置"), QString::fromUtf8("数据浏览")
    };
    for (int i = 0; i < navTexts.size(); ++i) {
        auto *button = new QPushButton(navTexts.at(i), sideBar);
        button->setObjectName("navButton");
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setMinimumHeight(58);
        sideLayout->addWidget(button);
        m_navigation.append(button);
        connect(button, &QPushButton::clicked, this, [this, i]() { switchPage(i); });
    }
    sideLayout->addStretch();
    rootLayout->addWidget(sideBar);

    auto *workspace = new QVBoxLayout;
    workspace->setContentsMargins(0, 0, 0, 0);
    workspace->setSpacing(0);
    auto *topBar = new QFrame(root);
    topBar->setObjectName("topBar");
    topBar->setFixedHeight(64);
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(20, 0, 20, 0);
    m_pageTitle = new QLabel(topBar);
    m_pageTitle->setObjectName("pageTitle");
    topLayout->addWidget(m_pageTitle);
    topLayout->addStretch();
    m_systemState = new QLabel(QString::fromUtf8("●  通信检查中"), topBar);
    m_systemState->setObjectName("systemPill");
    topLayout->addWidget(m_systemState);
    m_clock = new QLabel(topBar);
    m_clock->setObjectName("clock");
    topLayout->addSpacing(16);
    topLayout->addWidget(m_clock);
    workspace->addWidget(topBar);

    m_pages = new QStackedWidget(root);
    m_manualPanel = new ManualPanel(&m_deviceManager, m_pages);
    m_autoPanel = new AutoPanel(&m_deviceManager, m_pages);
    m_settingsWidget = new SettingsWidget(&m_rotator, m_pages);
    m_historyWidget = new HistoryWidget(&m_historyQuery, &m_deviceManager, m_pages);
    m_pages->addWidget(m_manualPanel);
    m_pages->addWidget(m_autoPanel);
    m_pages->addWidget(m_settingsWidget);
    m_pages->addWidget(m_historyWidget);

    auto *pageMargin = new QWidget(root);
    auto *pageLayout = new QVBoxLayout(pageMargin);
    pageLayout->setContentsMargins(14, 12, 14, 10);
    pageLayout->addWidget(m_pages);
    workspace->addWidget(pageMargin, 1);

    m_statusBar = new QLabel(QString::fromUtf8("系统初始化中…"), root);
    m_statusBar->setObjectName("bottomStatus");
    m_statusBar->setFixedHeight(28);
    workspace->addWidget(m_statusBar);
    rootLayout->addLayout(workspace, 1);
    setCentralWidget(root);

    connect(m_manualPanel, &ManualPanel::writeRequested,
            this, &MainWindow::writeToDevice);
    connect(m_autoPanel, &AutoPanel::runningChanged,
            this, &MainWindow::setAutomaticRunning);
    connect(m_settingsWidget, &SettingsWidget::settingsSaved,
            this, &MainWindow::onSettingsSaved);
    connect(m_settingsWidget, &SettingsWidget::rescanRequested,
            this, &MainWindow::rescanDevices);
    connect(m_settingsWidget, &SettingsWidget::dataFilesChanged,
            m_historyWidget, &HistoryWidget::onDataFilesChanged);

    auto *clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, &MainWindow::updateClock);
    clockTimer->start(1000);
    updateClock();
}

void MainWindow::startServices()
{
    const AppConfig &config = AppConfig::instance();
    m_configError = config.validationErrors().join(QString::fromUtf8("；"));
    m_deviceManager.clearDiscoveredDevices();
    m_logger.setDataPath(config.general().dataPath);
    m_historyQuery.setDataPath(config.general().dataPath);
    m_rotator.setDataPath(config.general().dataPath);
    m_rotator.setRetentionDays(config.general().retentionDays);
    m_rotator.setMaxStorageMB(config.general().maxStorageMB);
    m_rotator.start();
    m_settingsWidget->refreshStorageInfo();

    m_scheduler = new PollScheduler(&m_deviceManager, &m_logger, this);
    connect(m_scheduler, &PollScheduler::writeCompleted,
            this, &MainWindow::onWriteCompleted);
    connect(m_scheduler, &PollScheduler::schedulerError,
            this, [this](const QString &message) {
                m_schedulerFault = true;
                m_statusBar->setText(message);
                refreshSystemState();
            });
    connect(&m_deviceManager, &DeviceManager::deviceUpdated,
            this, &MainWindow::onDeviceUpdated);
    connect(&m_logger, &DataLogger::recordAppended,
            m_historyWidget, &HistoryWidget::onRecordAppended);

    const bool schedulerStarted = m_scheduler->start();
    m_statusBar->setText(!m_configError.isEmpty()
        ? m_configError
        : schedulerStarted ? QString::fromUtf8("通信已启动，正在自动搜索子板")
                           : QString::fromUtf8("轮询未启动，请检查串口配置"));
    m_schedulerFault = !schedulerStarted;
    refreshSystemState();
}

void MainWindow::switchPage(int index)
{
    if (index < 0 || index >= m_pages->count())
        return;
    const QStringList titles = {
        QString::fromUtf8("手动操作"), QString::fromUtf8("自动运行"),
        QString::fromUtf8("参数设置"), QString::fromUtf8("历史数据浏览")
    };
    m_pages->setCurrentIndex(index);
    m_pageTitle->setText(titles.at(index));
    m_navigation.at(index)->setChecked(true);

    if (index == 0) {
        if (m_autoRunning)
            setAutomaticRunning(false);
        if (!m_highVoltageAlarm)
            setAllIndicatorLights(0, 1, 0, 0);
        m_statusBar->setText(QString::fromUtf8("已进入手动模式，子板黄灯点亮"));
    } else if (index == 3) {
        m_historyWidget->activate();
    } else if (index == 2) {
        m_settingsWidget->refreshStorageInfo();
    }
}

void MainWindow::writeToDevice(const DeviceProfile::DeviceKey &key,
                               const QMap<QString, QVariant> &fields)
{
    if (!m_scheduler || !m_deviceManager.hasDevice(key))
        return;
    const QStringList spareOutputs = { "ot01", "ot02", "ot05", "ot06" };
    for (const QString &field : spareOutputs) {
        if (fields.contains(field)) {
            m_statusBar->setText(QString::fromUtf8(
                "备用输出 %1 采用安全关闭策略，已拦截操作").arg(field.toUpper()));
            return;
        }
    }
    if (m_highVoltageAlarm && (fields.contains("ot03") || fields.contains("ot04"))) {
        m_statusBar->setText(QString::fromUtf8("高压告警中，已拦截 OT3 / OT4 操作"));
        return;
    }
    if (m_sensorFaults.contains(commandKey(key))
        && (fields.contains("ot03") || fields.contains("ot04"))) {
        m_statusBar->setText(QString::fromUtf8(
            "该子板温湿度自检异常，已拦截 OT3 / OT4 操作"));
        return;
    }
    m_scheduler->writeToDevice(key, fields);
    m_statusBar->setText(QString::fromUtf8("正在向 ID %1 发送指令…").arg(key.slaveId));
}

void MainWindow::setAutomaticRunning(bool running)
{
    if (running && m_highVoltageAlarm) {
        m_statusBar->setText(QString::fromUtf8("高压告警未解除，无法启动自动运行"));
        m_autoPanel->setRunning(false);
        return;
    }
    if (running && !m_sensorFaults.isEmpty()) {
        m_statusBar->setText(QString::fromUtf8(
            "仍有 %1 块子板温湿度自检异常，无法启动自动运行")
            .arg(m_sensorFaults.size()));
        m_autoPanel->setRunning(false);
        return;
    }
    if (running) {
        const QList<DeviceState> devices = m_deviceManager.allDevices();
        int healthyCount = 0;
        for (const DeviceState &state : devices) {
            if (state.online && m_sensorHealthyDevices.contains(commandKey(state.key)))
                ++healthyCount;
        }
        if (devices.isEmpty() || healthyCount != devices.size()) {
            m_statusBar->setText(devices.isEmpty()
                ? QString::fromUtf8("尚未发现子板，无法启动自动运行")
                : QString::fromUtf8("温湿度自检尚未全部通过（%1/%2），无法启动")
                      .arg(healthyCount).arg(devices.size()));
            m_autoPanel->setRunning(false);
            return;
        }
    }
    m_autoRunning = running;
    m_autoPanel->setRunning(running);
    m_lastAutoCommands.clear();
    m_lastAutoCommandTimes.clear();
    m_pidStates.clear();
    if (running) {
        setAllIndicatorLights(1, 0, 0, 0);
        for (const DeviceState &state : m_deviceManager.allDevices())
            applyAutomaticControl(state.key);
        m_statusBar->setText(QString::fromUtf8("自动温控已启动，子板绿灯点亮"));
    } else {
        setAllIndicatorLights(0, 0, 0, 0);
        m_statusBar->setText(QString::fromUtf8("自动温控已停止"));
    }
    refreshSystemState();
}

void MainWindow::onDeviceUpdated(const DeviceProfile::DeviceKey &key)
{
    const DeviceState updatedState = m_deviceManager.device(key);
    if (!updatedState.online) {
        m_initializedDevices.remove(commandKey(key));
        m_sensorHealthyDevices.remove(commandKey(key));
    }
    if (updatedState.online && !m_initializedDevices.contains(commandKey(key)))
        initializeSafeOutputs(key);
    if (updatedState.online)
        evaluateSensorSelfCheck(key, updatedState);

    bool anyHighVoltage = false;
    for (const DeviceState &state : m_deviceManager.allDevices()) {
        if (hasHighVoltage(state.values)) {
            anyHighVoltage = true;
            break;
        }
    }
    if (anyHighVoltage && !m_highVoltageAlarm)
        enterHighVoltageAlarm();
    else if (!anyHighVoltage && m_highVoltageAlarm)
        leaveHighVoltageAlarm();

    if (m_autoRunning && !m_highVoltageAlarm)
        applyAutomaticControl(key);
    refreshSystemState();
}

void MainWindow::onWriteCompleted(const DeviceProfile::DeviceKey &key,
                                  bool success,
                                  const QString &error)
{
    if (!success) {
        m_lastAutoCommands.remove(commandKey(key));
        m_lastAutoCommandTimes.remove(commandKey(key));
    }
    m_statusBar->setText(success
        ? QString::fromUtf8("ID %1 指令执行成功").arg(key.slaveId)
        : QString::fromUtf8("ID %1 写入失败：%2").arg(key.slaveId).arg(error));
}

void MainWindow::onSettingsSaved()
{
    const AppConfig::GeneralConfig &config = AppConfig::instance().general();
    m_autoPanel->refreshParameters();
    m_logger.setDataPath(config.dataPath);
    m_historyQuery.setDataPath(config.dataPath);
    m_rotator.setDataPath(config.dataPath);
    m_rotator.setRetentionDays(config.retentionDays);
    m_rotator.setMaxStorageMB(config.maxStorageMB);
    m_settingsWidget->refreshStorageInfo();
    m_statusBar->setText(QString::fromUtf8("参数已保存并应用"));
    m_lastAutoCommands.clear();
    m_lastAutoCommandTimes.clear();
    m_pidStates.clear();
}

void MainWindow::rescanDevices()
{
    if (m_highVoltageAlarm) {
        m_statusBar->setText(QString::fromUtf8("高压告警中，请先确认现场安全再重新扫描"));
        m_settingsWidget->showActionFeedback(
            QString::fromUtf8("⚠  高压告警中，未开始扫描"), false);
        return;
    }
    if (!m_scheduler) {
        m_settingsWidget->showActionFeedback(
            QString::fromUtf8("⚠  通信未启动，无法扫描"), false);
        return;
    }
    if (m_autoRunning)
        setAutomaticRunning(false);
    m_scheduler->rescanDevices();
    m_lastAutoCommands.clear();
    m_lastAutoCommandTimes.clear();
    m_pidStates.clear();
    m_sensorFaults.clear();
    m_sensorRecoveryCounts.clear();
    m_sensorHealthyDevices.clear();
    m_initializedDevices.clear();
    m_startedAt = QDateTime::currentDateTime();
    m_statusBar->setText(QString::fromUtf8("正在重新扫描两路 RS485 子板…"));
    m_settingsWidget->showActionFeedback(
        QString::fromUtf8("✓  已开始重新扫描子板"), true);
    refreshSystemState();
}

void MainWindow::updateClock()
{
    m_clock->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd  hh:mm:ss"));
}

void MainWindow::setAllIndicatorLights(int green, int yellow, int red, int buzzer)
{
    if (!m_scheduler)
        return;
    QMap<QString, QVariant> fields;
    fields["ot07"] = green;
    fields["ot08"] = yellow;
    fields["ot09"] = red;
    fields["ot10"] = buzzer;
    fields["ot01"] = 0;
    fields["ot02"] = 0;
    fields["ot05"] = 0;
    fields["ot06"] = 0;
    for (const DeviceState &state : m_deviceManager.allDevices()) {
        QMap<QString, QVariant> deviceFields = fields;
        if (!m_highVoltageAlarm && m_sensorFaults.contains(commandKey(state.key))) {
            deviceFields["ot07"] = 0;
            deviceFields["ot08"] = 0;
            deviceFields["ot09"] = 1;
            deviceFields["ot10"] = 0;
        }
        m_scheduler->writeToDevice(state.key, deviceFields);
    }
}

void MainWindow::applyAutomaticControl(const DeviceProfile::DeviceKey &key)
{
    const int keyValue = commandKey(key);
    if (!m_autoRunning || m_highVoltageAlarm || m_sensorFaults.contains(keyValue)
        || !m_deviceManager.hasDevice(key))
        return;
    const DeviceState state = m_deviceManager.device(key);
    if (!state.online || !state.values.contains("th1_temp")
        || !state.values.contains("th2_temp") || !state.values.contains("th3_temp"))
        return;

    const double temperature = averageField(
        state.values, { "th1_temp", "th2_temp", "th3_temp" });
    const AppConfig::GeneralConfig &config = AppConfig::instance().general();
    ControlAlgorithm::ControlOutput control;
    if (config.temperatureControlMode == "pid") {
        ControlAlgorithm::PidConfig pidConfig;
        pidConfig.kp = config.pidKp;
        pidConfig.ki = config.pidKi;
        pidConfig.kd = config.pidKd;
        pidConfig.singleStagePercent = config.pidSingleStagePercent;
        pidConfig.dualStagePercent = config.pidDualStagePercent;
        control = ControlAlgorithm::pidControl(
            temperature, config.temperatureTarget,
            QDateTime::currentMSecsSinceEpoch(), pidConfig, m_pidStates[keyValue]);
    } else {
        control = ControlAlgorithm::thresholdControl(
            temperature, config.temperatureTarget,
            config.lowerHysteresis, config.upperHysteresis,
            state.values.value("ot03").toInt(),
            state.values.value("ot04").toInt());
    }
    if (!control.hasCommand)
        return;

    const QPair<int, int> desired(control.ot3, control.ot4);
    if (m_lastAutoCommands.value(keyValue, QPair<int, int>(-1, -1)) == desired)
        return;
    const QDateTime now = QDateTime::currentDateTime();
    if (m_lastAutoCommandTimes.contains(keyValue)
        && m_lastAutoCommandTimes.value(keyValue).secsTo(now)
            < qMax(1, config.relaySwitchIntervalSec))
        return;
    m_lastAutoCommands[keyValue] = desired;
    m_lastAutoCommandTimes[keyValue] = now;
    QMap<QString, QVariant> fields;
    fields["ot03"] = control.ot3;
    fields["ot04"] = control.ot4;
    m_scheduler->writeToDevice(key, fields);
}

void MainWindow::evaluateSensorSelfCheck(const DeviceProfile::DeviceKey &key,
                                         const DeviceState &state)
{
    const bool graceExpired = m_startedAt.isValid()
        && m_startedAt.secsTo(QDateTime::currentDateTime())
            >= qMax(1, AppConfig::instance().general().selfCheckGraceSec);
    const ControlAlgorithm::SensorCheck check =
        ControlAlgorithm::checkTemperatureHumidity(
            state.values, configuredSensorLimits(), graceExpired);
    const int keyValue = commandKey(key);
    if (check.state == ControlAlgorithm::SensorCheck::Fault) {
        m_sensorHealthyDevices.remove(keyValue);
        m_sensorRecoveryCounts.remove(keyValue);
        enterSensorFault(key, check.message);
    } else if (check.state == ControlAlgorithm::SensorCheck::Healthy) {
        m_sensorHealthyDevices.insert(keyValue);
        if (m_sensorFaults.contains(keyValue)) {
            const int validCount = m_sensorRecoveryCounts.value(keyValue) + 1;
            m_sensorRecoveryCounts[keyValue] = validCount;
            if (validCount >= 3)
                leaveSensorFault(key);
        } else {
            m_sensorRecoveryCounts.remove(keyValue);
        }
    } else {
        m_sensorHealthyDevices.remove(keyValue);
    }
}

void MainWindow::enterSensorFault(const DeviceProfile::DeviceKey &key,
                                  const QString &reason)
{
    const int keyValue = commandKey(key);
    const bool newFault = !m_sensorFaults.contains(keyValue);
    m_sensorFaults[keyValue] = reason;
    if (!newFault)
        return;

    if (m_autoRunning) {
        m_autoRunning = false;
        m_autoPanel->setRunning(false);
        m_lastAutoCommands.clear();
        m_lastAutoCommandTimes.clear();
        m_pidStates.clear();
        stopAllControlledOutputs();
    }
    if (m_scheduler) {
        QMap<QString, QVariant> fields;
        fields["ot01"] = 0;
        fields["ot02"] = 0;
        fields["ot03"] = 0;
        fields["ot04"] = 0;
        fields["ot05"] = 0;
        fields["ot06"] = 0;
        fields["ot07"] = 0;
        fields["ot08"] = 0;
        fields["ot09"] = 1;
        fields["ot10"] = 0;
        m_scheduler->writeToDevice(key, fields);
    }
    m_statusBar->setText(QString::fromUtf8("ID %1 温湿度自检异常：%2")
                             .arg(key.slaveId).arg(reason));
    QTimer::singleShot(0, this, [this, key, reason]() {
        QMessageBox::critical(this, QString::fromUtf8("温湿度自检异常"),
            QString::fromUtf8("端口 %1 / 子板 ID %2\n%3\n\n"
                              "已停止自动温控、关闭 OT3/OT4，并点亮该子板红灯。")
                .arg(key.portIndex + 1).arg(key.slaveId).arg(reason));
    });
}

void MainWindow::leaveSensorFault(const DeviceProfile::DeviceKey &key)
{
    const int keyValue = commandKey(key);
    m_sensorFaults.remove(keyValue);
    m_sensorRecoveryCounts.remove(keyValue);
    m_pidStates.remove(keyValue);
    if (m_scheduler && !m_highVoltageAlarm) {
        QMap<QString, QVariant> fields;
        fields["ot01"] = 0;
        fields["ot02"] = 0;
        fields["ot05"] = 0;
        fields["ot06"] = 0;
        fields["ot07"] = m_autoRunning ? 1 : 0;
        fields["ot08"] = !m_autoRunning && m_pages->currentIndex() == 0 ? 1 : 0;
        fields["ot09"] = 0;
        fields["ot10"] = 0;
        m_scheduler->writeToDevice(key, fields);
    }
    m_statusBar->setText(QString::fromUtf8(
        "ID %1 温湿度连续 3 次正常，自检告警已解除").arg(key.slaveId));
}

void MainWindow::stopAllControlledOutputs()
{
    if (!m_scheduler)
        return;
    QMap<QString, QVariant> fields;
    fields["ot01"] = 0;
    fields["ot02"] = 0;
    fields["ot03"] = 0;
    fields["ot04"] = 0;
    fields["ot05"] = 0;
    fields["ot06"] = 0;
    for (const DeviceState &state : m_deviceManager.allDevices())
        m_scheduler->writeToDevice(state.key, fields);
}

void MainWindow::initializeSafeOutputs(const DeviceProfile::DeviceKey &key)
{
    if (!m_scheduler)
        return;
    m_initializedDevices.insert(commandKey(key));
    QMap<QString, QVariant> fields;
    fields["ot01"] = 0;
    fields["ot02"] = 0;
    fields["ot05"] = 0;
    fields["ot06"] = 0;
    fields["ot07"] = !m_highVoltageAlarm && m_autoRunning ? 1 : 0;
    fields["ot08"] = !m_highVoltageAlarm && !m_autoRunning
        && m_pages->currentIndex() == 0 ? 1 : 0;
    fields["ot09"] = m_highVoltageAlarm ? 1 : 0;
    fields["ot10"] = m_highVoltageAlarm ? 1 : 0;
    m_scheduler->writeToDevice(key, fields);
}

void MainWindow::enterHighVoltageAlarm()
{
    m_highVoltageAlarm = true;
    m_autoRunning = false;
    m_autoPanel->setRunning(false);
    m_lastAutoCommands.clear();
    m_lastAutoCommandTimes.clear();
    if (m_scheduler) {
        QMap<QString, QVariant> fields;
        fields["ot03"] = 0;
        fields["ot04"] = 0;
        fields["ot07"] = 0;
        fields["ot08"] = 0;
        fields["ot09"] = 1;
        fields["ot10"] = 1;
        fields["ot01"] = 0;
        fields["ot02"] = 0;
        fields["ot05"] = 0;
        fields["ot06"] = 0;
        for (const DeviceState &state : m_deviceManager.allDevices())
            m_scheduler->writeToDevice(state.key, fields);
    }
    refreshSystemState();
    m_statusBar->setText(QString::fromUtf8("高压带电：已切断全部 OT3 / OT4 并启动声光告警"));
}

void MainWindow::leaveHighVoltageAlarm()
{
    m_highVoltageAlarm = false;
    setAllIndicatorLights(0, m_pages->currentIndex() == 0 ? 1 : 0, 0, 0);
    refreshSystemState();
    m_statusBar->setText(QString::fromUtf8("高压告警已解除，请确认现场安全后继续操作"));
}

void MainWindow::refreshSystemState()
{
    const QList<DeviceState> devices = m_deviceManager.allDevices();
    int checkedCount = 0;
    int onlineCount = 0;
    for (const DeviceState &state : devices) {
        if (state.lastUpdate.isValid())
            ++checkedCount;
        if (state.online)
            ++onlineCount;
    }

    bool alarm = false;
    bool neutral = false;
    if (m_highVoltageAlarm) {
        m_systemState->setText(QString::fromUtf8("⚠  高压告警"));
        alarm = true;
    } else if (!m_configError.isEmpty()) {
        m_systemState->setText(QString::fromUtf8("⚠  配置异常"));
        alarm = true;
    } else if (m_schedulerFault) {
        m_systemState->setText(QString::fromUtf8("⚠  通信异常"));
        alarm = true;
    } else if (!m_sensorFaults.isEmpty()) {
        m_systemState->setText(QString::fromUtf8("⚠  温湿度自检异常 %1 块")
                                   .arg(m_sensorFaults.size()));
        alarm = true;
    } else if (devices.isEmpty()) {
        m_systemState->setText(QString::fromUtf8("●  正在搜索子板"));
        neutral = true;
    } else if (checkedCount < devices.size()) {
        m_systemState->setText(QString::fromUtf8("●  通信检查中"));
        neutral = true;
    } else if (onlineCount == 0) {
        m_systemState->setText(QString::fromUtf8("⚠  全部子板离线"));
        alarm = true;
    } else if (onlineCount < devices.size()) {
        m_systemState->setText(QString::fromUtf8("⚠  子板在线 %1/%2")
                                   .arg(onlineCount).arg(devices.size()));
        alarm = true;
    } else if (m_startedAt.isValid()
               && m_startedAt.secsTo(QDateTime::currentDateTime())
                   < qMax(1, AppConfig::instance().general().selfCheckGraceSec)) {
        m_systemState->setText(QString::fromUtf8("●  温湿度自检中"));
        neutral = true;
    } else if (m_autoRunning) {
        m_systemState->setText(QString::fromUtf8("●  自动运行"));
    } else {
        m_systemState->setText(QString::fromUtf8("●  系统正常"));
    }
    m_systemState->setProperty("alarm", alarm);
    m_systemState->setProperty("neutral", neutral);
    refreshDynamicStyle(m_systemState);
}
