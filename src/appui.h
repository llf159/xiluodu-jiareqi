#ifndef APPUI_H
#define APPUI_H

#include "applogic.h"
#include "controlalgorithm.h"

#include <QMainWindow>
#include <QMap>
#include <QPoint>
#include <QSet>
#include <QVector>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QMouseEvent;
class QPushButton;
class QScrollBar;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTimer;

/** 手动和自动页共用的子板选择与传感器数据卡片。 */
class DeviceOverviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit DeviceOverviewWidget(DeviceManager *manager, QWidget *parent = nullptr);

    DeviceProfile::DeviceKey currentDevice() const;
    DeviceState currentState() const;
    void selectDevice(const DeviceProfile::DeviceKey &key);

signals:
    void currentDeviceChanged(const DeviceProfile::DeviceKey &key);

public slots:
    void refreshDevices();
    void refreshDevice(const DeviceProfile::DeviceKey &key);

private:
    void refreshValues();

    DeviceManager *m_manager = nullptr;
    QLabel *m_deviceTitle = nullptr;
    QComboBox *m_deviceBox = nullptr;
    QLabel *m_linkState = nullptr;
    QLabel *m_lastUpdate = nullptr;
    QMap<QString, QLabel *> m_values;
};

/** 手动操作：选择子板、查看状态、单独切换 OT3/OT4。 */
class ManualPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ManualPanel(DeviceManager *manager, QWidget *parent = nullptr);

    DeviceProfile::DeviceKey currentDevice() const;
    void setCurrentDevice(const DeviceProfile::DeviceKey &key);
    void refreshSettings();

signals:
    void writeRequested(const DeviceProfile::DeviceKey &key,
                        const QMap<QString, QVariant> &fields);

private slots:
    void refreshControls();
    void toggleOutput();

private:
    DeviceManager *m_manager = nullptr;
    DeviceOverviewWidget *m_overview = nullptr;
    QLabel *m_operationBanner = nullptr;
    QPushButton *m_ot3 = nullptr;
    QPushButton *m_ot4 = nullptr;
    QMap<QString, QPushButton *> m_spareOutputs;
};

/** 自动运行：状态监视、阈值摘要和启停。 */
class AutoPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AutoPanel(DeviceManager *manager, QWidget *parent = nullptr);

    void setRunning(bool running);
    void refreshParameters();
    void setCurrentDevice(const DeviceProfile::DeviceKey &key);

signals:
    void runningChanged(bool running);

private slots:
    void refreshState();

private:
    DeviceManager *m_manager = nullptr;
    DeviceOverviewWidget *m_overview = nullptr;
    QLabel *m_runState = nullptr;
    QLabel *m_workState = nullptr;
    QLabel *m_averageTemp = nullptr;
    QLabel *m_ruleText = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    bool m_running = false;
};

/** 历史数据折线图，不引入 Qt Charts，便于 RK3568 部署。 */
class HistoryChart : public QWidget
{
    Q_OBJECT
public:
    explicit HistoryChart(QWidget *parent = nullptr);
    void setRecords(const QVector<HistoryQuery::Record> &records);
    void appendRecord(const HistoryQuery::Record &record);
    int recordCount() const { return m_records.size(); }
    int visiblePointCount() const;
    int maxViewStart() const;
    int viewStart() const { return m_viewStart; }
    void followLatest();

public slots:
    void setViewStart(int start);

signals:
    void viewStartChanged(int start);
    void inspectionChanged(const QString &summary);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void inspectAtX(int x);
    void refreshAfterRecordsChanged();

    QVector<HistoryQuery::Record> m_records;
    int m_viewStart = 0;
    int m_crosshairRecordIndex = -1;
    int m_dragViewStart = 0;
    QPoint m_dragOrigin;
    bool m_dragging = false;
    bool m_followLatest = true;
};

/** 历史数据：按日期和子板查询，同时显示曲线与明细。 */
class HistoryWidget : public QWidget
{
    Q_OBJECT
public:
    explicit HistoryWidget(HistoryQuery *query,
                           DeviceManager *manager,
                           QWidget *parent = nullptr);

public slots:
    void activate();
    void refreshDevices();
    void reloadDevices();
    void onDataFilesChanged();
    void queryRecords();
    void onRecordAppended(const QDateTime &timestamp,
                          const DeviceProfile::DeviceKey &key,
                          const QString &deviceName,
                          DeviceProfile::DeviceType type,
                          const QMap<QString, QVariant> &values);

private:
    void loadRecords(bool resetToLatest);
    void updateTimeScroll();
    void setTableRow(int row, const HistoryQuery::Record &record);

    HistoryQuery *m_query = nullptr;
    DeviceManager *m_manager = nullptr;
    QDateEdit *m_dateFrom = nullptr;
    QDateEdit *m_dateTo = nullptr;
    QComboBox *m_deviceBox = nullptr;
    QLabel *m_resultSummary = nullptr;
    QLabel *m_crosshairInfo = nullptr;
    HistoryChart *m_chart = nullptr;
    QScrollBar *m_timeScroll = nullptr;
    QTableWidget *m_table = nullptr;
    QMap<QPair<int, int>, QString> m_historicalDevices;
    QVector<HistoryQuery::Record> m_chartRecords;
    int m_rawRecordCount = 0;
    bool m_loaded = false;
    bool m_deviceIndexLoaded = false;
    QDate m_loadedDateFrom;
    QDate m_loadedDateTo;
    QString m_loadedDevice;
};

/** 参数设置：温控参数，其他运维项收纳在高级设置。 */
class SettingsWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsWidget(StorageRotator *rotator, QWidget *parent = nullptr);
    void showActionFeedback(const QString &message, bool success);
    void refreshStorageInfo();

signals:
    void settingsSaved();
    void rescanRequested();
    void dataFilesChanged();

private slots:
    void saveSettings();
    void deleteOldData();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;

    StorageRotator *m_rotator = nullptr;
    QComboBox *m_controlMode = nullptr;
    QComboBox *m_targetSource = nullptr;
    QDoubleSpinBox *m_targetTemp = nullptr;
    QDoubleSpinBox *m_thresholdSingleStage = nullptr;
    QDoubleSpinBox *m_thresholdSecondStage = nullptr;
    QDoubleSpinBox *m_thresholdDualStage = nullptr;
    QDoubleSpinBox *m_thresholdHysteresis = nullptr;
    QDoubleSpinBox *m_pidKp = nullptr;
    QDoubleSpinBox *m_pidKi = nullptr;
    QDoubleSpinBox *m_pidKd = nullptr;
    QDoubleSpinBox *m_pidSingleStage = nullptr;
    QDoubleSpinBox *m_pidSecondStage = nullptr;
    QDoubleSpinBox *m_pidDualStage = nullptr;
    QComboBox *m_pidFirstStageOutput = nullptr;
    QDoubleSpinBox *m_dewPointSingleStage = nullptr;
    QDoubleSpinBox *m_dewPointSecondStage = nullptr;
    QDoubleSpinBox *m_dewPointDualStage = nullptr;
    QDoubleSpinBox *m_dewPointHysteresis = nullptr;
    QDoubleSpinBox *m_humidityTemperatureLimit = nullptr;
    QLabel *m_humidityFormula = nullptr;
    QComboBox *m_highVoltageDetectionMode = nullptr;
    QComboBox *m_highVoltageDigitalTrigger = nullptr;
    QDoubleSpinBox *m_highVoltageThreshold = nullptr;
    QSpinBox *m_relaySwitchInterval = nullptr;
    QSpinBox *m_recordInterval = nullptr;
    QSpinBox *m_maxStorageGB = nullptr;
    QSpinBox *m_deleteAge = nullptr;
    QComboBox *m_deleteAgeUnit = nullptr;
    QMap<int, QComboBox *> m_spareOutputModes;
    QComboBox *m_reservedInputMode = nullptr;
    QLabel *m_storageSummary = nullptr;
    QLabel *m_deleteDateReference = nullptr;
    QLabel *m_formula = nullptr;
    QLabel *m_actionFeedback = nullptr;
    QTimer *m_feedbackTimer = nullptr;

    QDate deleteCutoffDate() const;
};

/** 1024x600 触摸屏主窗口。 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void switchPage(int index);
    void writeToDevice(const DeviceProfile::DeviceKey &key,
                       const QMap<QString, QVariant> &fields);
    void setAutomaticRunning(bool running);
    void onDeviceUpdated(const DeviceProfile::DeviceKey &key);
    void onWriteCompleted(const DeviceProfile::DeviceKey &key,
                          bool success,
                          const QString &error);
    void onSettingsSaved();
    void rescanDevices();
    void updateClock();

private:
    void setupUi();
    void startServices();
    void setAllIndicatorLights(int green, int yellow, int red, int buzzer);
    void applyAutomaticControl(const DeviceProfile::DeviceKey &key);
    void enterHighVoltageAlarm();
    void leaveHighVoltageAlarm();
    void evaluateSensorSelfCheck(const DeviceProfile::DeviceKey &key,
                                 const DeviceState &state);
    void enterSensorFault(const DeviceProfile::DeviceKey &key,
                          const QString &reason);
    void leaveSensorFault(const DeviceProfile::DeviceKey &key);
    void stopAllControlledOutputs();
    void initializeSafeOutputs(const DeviceProfile::DeviceKey &key);
    void refreshHighVoltageAlarm();
    void addConfiguredSpareOutputs(QMap<QString, QVariant> &fields,
                                   const QMap<QString, QVariant> &currentValues =
                                       QMap<QString, QVariant>(),
                                   bool initializeManual = false) const;
    void evaluateReservedInput(const DeviceProfile::DeviceKey &key,
                               const DeviceState &state);
    void enterReservedInputInterlock(const DeviceProfile::DeviceKey &key,
                                     int value);
    void leaveReservedInputInterlock(const DeviceProfile::DeviceKey &key);
    void refreshSystemState();

    DeviceManager m_deviceManager;
    DataLogger m_logger;
    HistoryQuery m_historyQuery;
    StorageRotator m_rotator;
    PollScheduler *m_scheduler = nullptr;

    QStackedWidget *m_pages = nullptr;
    QVector<QPushButton *> m_navigation;
    ManualPanel *m_manualPanel = nullptr;
    AutoPanel *m_autoPanel = nullptr;
    SettingsWidget *m_settingsWidget = nullptr;
    HistoryWidget *m_historyWidget = nullptr;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_clock = nullptr;
    QLabel *m_systemState = nullptr;
    QLabel *m_selfCheckNotice = nullptr;
    QLabel *m_statusBar = nullptr;
    bool m_autoRunning = false;
    bool m_highVoltageAlarm = false;
    bool m_schedulerFault = false;
    QString m_configError;
    QDateTime m_startedAt;
    QMap<int, QPair<int, int>> m_lastAutoCommands;
    QMap<int, QDateTime> m_lastAutoCommandTimes;
    QMap<int, ControlAlgorithm::PidState> m_pidStates;
    QMap<int, QString> m_sensorFaults;
    QMap<int, int> m_sensorRecoveryCounts;
    QSet<int> m_sensorHealthyDevices;
    QMap<int, QString> m_reservedInputInterlocks;
    QSet<int> m_initializedDevices;
};

#endif // APPUI_H
