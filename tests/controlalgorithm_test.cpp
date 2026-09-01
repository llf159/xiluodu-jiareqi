#include <QtTest>

#include "controlalgorithm.h"

class ControlAlgorithmTest : public QObject
{
    Q_OBJECT

private slots:
    void sensorHealthAcceptsNormalReadings();
    void sensorHealthAllowsDifferentLocations();
    void sensorHealthRejectsOutOfRangeValue();
    void sensorHealthWaitsThenFailsOnMissingData();
    void thresholdModeMatchesRequirement();
    void pidModeProducesSafeStages();
    void pidStageThresholdsAreConfigurable();
};

static QMap<QString, QVariant> normalValues()
{
    QMap<QString, QVariant> values;
    values["th1_temp"] = 250;
    values["th2_temp"] = 255;
    values["th3_temp"] = 248;
    values["th1_humi"] = 500;
    values["th2_humi"] = 520;
    values["th3_humi"] = 510;
    return values;
}

void ControlAlgorithmTest::sensorHealthAcceptsNormalReadings()
{
    const auto result = ControlAlgorithm::checkTemperatureHumidity(
        normalValues(), ControlAlgorithm::SensorLimits(), true);
    QCOMPARE(result.state, ControlAlgorithm::SensorCheck::Healthy);
}

void ControlAlgorithmTest::sensorHealthAllowsDifferentLocations()
{
    auto values = normalValues();
    values["th1_temp"] = -300;
    values["th2_temp"] = 250;
    values["th3_temp"] = 800;
    const auto result = ControlAlgorithm::checkTemperatureHumidity(
        values, ControlAlgorithm::SensorLimits(), false);
    QCOMPARE(result.state, ControlAlgorithm::SensorCheck::Healthy);
}

void ControlAlgorithmTest::sensorHealthRejectsOutOfRangeValue()
{
    auto values = normalValues();
    values["th3_temp"] = 900;
    const auto result = ControlAlgorithm::checkTemperatureHumidity(
        values, ControlAlgorithm::SensorLimits(), false);
    QCOMPARE(result.state, ControlAlgorithm::SensorCheck::Fault);
    QVERIFY(result.faultyFields.contains("th3_temp"));
}

void ControlAlgorithmTest::sensorHealthWaitsThenFailsOnMissingData()
{
    auto values = normalValues();
    values.remove("th2_humi");
    QCOMPARE(ControlAlgorithm::checkTemperatureHumidity(
        values, ControlAlgorithm::SensorLimits(), false).state,
        ControlAlgorithm::SensorCheck::Waiting);
    QCOMPARE(ControlAlgorithm::checkTemperatureHumidity(
        values, ControlAlgorithm::SensorLimits(), true).state,
        ControlAlgorithm::SensorCheck::Fault);
}

void ControlAlgorithmTest::thresholdModeMatchesRequirement()
{
    auto low = ControlAlgorithm::thresholdControl(20.0, 25.0, 2.0, 2.0, 0, 0);
    QVERIFY(low.hasCommand);
    QCOMPARE(low.ot3, 1);
    QCOMPARE(low.ot4, 1);

    auto middle = ControlAlgorithm::thresholdControl(26.0, 25.0, 2.0, 2.0, 0, 0);
    QCOMPARE(middle.ot3, 1);
    QCOMPARE(middle.ot4, 0);

    auto high = ControlAlgorithm::thresholdControl(28.0, 25.0, 2.0, 2.0, 1, 1);
    QCOMPARE(high.ot3, 0);
    QCOMPARE(high.ot4, 0);
}

void ControlAlgorithmTest::pidModeProducesSafeStages()
{
    ControlAlgorithm::PidConfig config;
    ControlAlgorithm::PidState state;
    auto full = ControlAlgorithm::pidControl(15.0, 25.0, 1000, config, state);
    QCOMPARE(full.ot3, 1);
    QCOMPARE(full.ot4, 1);
    QVERIFY(full.demandPercent <= 100.0);

    state = ControlAlgorithm::PidState();
    auto off = ControlAlgorithm::pidControl(35.0, 25.0, 1000, config, state);
    QCOMPARE(off.ot3, 0);
    QCOMPARE(off.ot4, 0);
    QVERIFY(off.demandPercent >= 0.0);
}

void ControlAlgorithmTest::pidStageThresholdsAreConfigurable()
{
    ControlAlgorithm::PidConfig config;
    config.kp = 10.0;
    config.ki = 0.0;
    config.kd = 0.0;
    config.singleStagePercent = 30.0;
    config.dualStagePercent = 80.0;

    ControlAlgorithm::PidState state;
    const auto single = ControlAlgorithm::pidControl(
        20.0, 25.0, 1000, config, state);
    QCOMPARE(single.ot3, 1);
    QCOMPARE(single.ot4, 0);

    state = ControlAlgorithm::PidState();
    const auto dual = ControlAlgorithm::pidControl(
        15.0, 25.0, 1000, config, state);
    QCOMPARE(dual.ot3, 1);
    QCOMPARE(dual.ot4, 1);
}

QTEST_APPLESS_MAIN(ControlAlgorithmTest)

#include "controlalgorithm_test.moc"
