#include <QtTest>
#include <cmath>

#include "controlalgorithm.h"

class ControlAlgorithmTest : public QObject
{
    Q_OBJECT

private slots:
    void sensorHealthAcceptsNormalReadings();
    void sensorHealthAllowsDifferentLocations();
    void sensorHealthRejectsOutOfRangeValue();
    void sensorHealthReportsExtremeValueWithMissingData();
    void sensorHealthKeepsWaitingOnMissingData();
    void thresholdModeProducesThreeHeatingStages();
    void thresholdModeUsesHysteresisAndOutputOrder();
    void dewPointMatchesKnownCondition();
    void condensationControlUsesMarginAndRecovery();
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
        normalValues(), ControlAlgorithm::SensorLimits());
    QCOMPARE(result.state, ControlAlgorithm::SensorCheck::Healthy);
}

void ControlAlgorithmTest::sensorHealthAllowsDifferentLocations()
{
    auto values = normalValues();
    values["th1_temp"] = -300;
    values["th2_temp"] = 250;
    values["th3_temp"] = 800;
    const auto result = ControlAlgorithm::checkTemperatureHumidity(
        values, ControlAlgorithm::SensorLimits());
    QCOMPARE(result.state, ControlAlgorithm::SensorCheck::Healthy);
}

void ControlAlgorithmTest::sensorHealthRejectsOutOfRangeValue()
{
    auto values = normalValues();
    values["th3_temp"] = 900;
    const auto result = ControlAlgorithm::checkTemperatureHumidity(
        values, ControlAlgorithm::SensorLimits());
    QCOMPARE(result.state, ControlAlgorithm::SensorCheck::Fault);
    QVERIFY(result.faultyFields.contains("th3_temp"));
}

void ControlAlgorithmTest::sensorHealthReportsExtremeValueWithMissingData()
{
    auto values = normalValues();
    values.remove("th2_humi");
    values["th3_temp"] = 900;
    const auto result = ControlAlgorithm::checkTemperatureHumidity(
        values, ControlAlgorithm::SensorLimits());
    QCOMPARE(result.state, ControlAlgorithm::SensorCheck::Fault);
    QVERIFY(result.faultyFields.contains("th3_temp"));
}

void ControlAlgorithmTest::sensorHealthKeepsWaitingOnMissingData()
{
    auto values = normalValues();
    values.remove("th2_humi");
    QCOMPARE(ControlAlgorithm::checkTemperatureHumidity(
        values, ControlAlgorithm::SensorLimits()).state,
        ControlAlgorithm::SensorCheck::Waiting);
}

void ControlAlgorithmTest::thresholdModeProducesThreeHeatingStages()
{
    ControlAlgorithm::ThresholdConfig config;
    auto single = ControlAlgorithm::thresholdControl(
        24.0, 25.0, config, 0, 0);
    QCOMPARE(single.ot3, 1);
    QCOMPARE(single.ot4, 0);

    auto second = ControlAlgorithm::thresholdControl(
        23.0, 25.0, config, 0, 0);
    QCOMPARE(second.ot3, 0);
    QCOMPARE(second.ot4, 1);

    auto dual = ControlAlgorithm::thresholdControl(
        20.0, 25.0, config, 0, 0);
    QCOMPARE(dual.ot3, 1);
    QCOMPARE(dual.ot4, 1);
}

void ControlAlgorithmTest::thresholdModeUsesHysteresisAndOutputOrder()
{
    ControlAlgorithm::ThresholdConfig config;
    auto heldOff = ControlAlgorithm::thresholdControl(
        24.6, 25.0, config, 0, 0);
    QVERIFY(!heldOff.hasCommand);

    auto exactSingleRise = ControlAlgorithm::thresholdControl(
        24.5, 25.0, config, 0, 0);
    QCOMPARE(exactSingleRise.ot3, 1);
    QCOMPARE(exactSingleRise.ot4, 0);

    auto heldSingle = ControlAlgorithm::thresholdControl(
        23.4, 25.0, config, 1, 0);
    QVERIFY(!heldSingle.hasCommand);
    QCOMPARE(heldSingle.ot3, 1);
    QCOMPARE(heldSingle.ot4, 0);

    auto exactSecondRise = ControlAlgorithm::thresholdControl(
        23.3, 25.0, config, 1, 0);
    QCOMPARE(exactSecondRise.ot3, 0);
    QCOMPARE(exactSecondRise.ot4, 1);

    auto exactDualRise = ControlAlgorithm::thresholdControl(
        21.8, 25.0, config, 0, 1);
    QCOMPARE(exactDualRise.ot3, 1);
    QCOMPARE(exactDualRise.ot4, 1);

    auto exactDualFall = ControlAlgorithm::thresholdControl(
        22.2, 25.0, config, 1, 1);
    QCOMPARE(exactDualFall.ot3, 0);
    QCOMPARE(exactDualFall.ot4, 1);

    auto exactOffFall = ControlAlgorithm::thresholdControl(
        24.9, 25.0, config, 1, 0);
    QCOMPARE(exactOffFall.ot3, 0);
    QCOMPARE(exactOffFall.ot4, 0);

    config.firstStageOt3 = false;
    auto swapped = ControlAlgorithm::thresholdControl(
        24.0, 25.0, config, 0, 0);
    QCOMPARE(swapped.ot3, 0);
    QCOMPARE(swapped.ot4, 1);
}

void ControlAlgorithmTest::dewPointMatchesKnownCondition()
{
    const double dewPoint = ControlAlgorithm::dewPointCelsius(20.0, 80.0);
    QVERIFY(qAbs(dewPoint - 16.4) < 0.1);
    QVERIFY(!std::isfinite(ControlAlgorithm::dewPointCelsius(20.0, 0.0)));
}

void ControlAlgorithmTest::condensationControlUsesMarginAndRecovery()
{
    ControlAlgorithm::CondensationConfig config;
    QCOMPARE(ControlAlgorithm::condensationHeatingStage(
                 20.0, 17.5, config, 0), 1);
    QCOMPARE(ControlAlgorithm::condensationHeatingStage(
                 20.0, 18.5, config, 0), 2);
    QCOMPARE(ControlAlgorithm::condensationHeatingStage(
                 20.0, 19.5, config, 0), 3);
    QCOMPARE(ControlAlgorithm::condensationHeatingStage(
                 20.0, 18.1, config, 2), 2);
    QCOMPARE(ControlAlgorithm::condensationHeatingStage(
                 20.0, 17.8, config, 2), 1);
    QCOMPARE(ControlAlgorithm::condensationHeatingStage(
                 20.0, 16.8, config, 1), 0);
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
    config.secondStagePercent = 60.0;
    config.dualStagePercent = 80.0;

    ControlAlgorithm::PidState state;
    const auto single = ControlAlgorithm::pidControl(
        20.0, 25.0, 1000, config, state);
    QCOMPARE(single.ot3, 1);
    QCOMPARE(single.ot4, 0);

    state = ControlAlgorithm::PidState();
    const auto second = ControlAlgorithm::pidControl(
        18.0, 25.0, 1000, config, state);
    QCOMPARE(second.ot3, 0);
    QCOMPARE(second.ot4, 1);

    state = ControlAlgorithm::PidState();
    const auto dual = ControlAlgorithm::pidControl(
        15.0, 25.0, 1000, config, state);
    QCOMPARE(dual.ot3, 1);
    QCOMPARE(dual.ot4, 1);

    config.firstStageOt3 = false;
    state = ControlAlgorithm::PidState();
    const auto swappedFirst = ControlAlgorithm::pidControl(
        20.0, 25.0, 1000, config, state);
    QCOMPARE(swappedFirst.ot3, 0);
    QCOMPARE(swappedFirst.ot4, 1);

    state = ControlAlgorithm::PidState();
    const auto swappedSecond = ControlAlgorithm::pidControl(
        18.0, 25.0, 1000, config, state);
    QCOMPARE(swappedSecond.ot3, 1);
    QCOMPARE(swappedSecond.ot4, 0);
}

QTEST_APPLESS_MAIN(ControlAlgorithmTest)

#include "controlalgorithm_test.moc"
