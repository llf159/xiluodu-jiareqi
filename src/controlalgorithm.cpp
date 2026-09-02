#include "controlalgorithm.h"

#include <QtGlobal>
#include <QVector>
#include <cmath>
#include <limits>

namespace {

constexpr double kThresholdComparisonEpsilon = 1e-9;

double engineeringValue(const QVariant &value)
{
    return value.toInt() / 10.0;
}

QString sensorLabel(const QString &field)
{
    const int channel = field.mid(2, 1).toInt();
    return field.endsWith("_temp")
        ? QString::fromUtf8("温度探头%1").arg(channel)
        : QString::fromUtf8("湿度探头%1").arg(channel);
}

bool outside(double value, double minimum, double maximum)
{
    return !std::isfinite(value) || value < minimum || value > maximum;
}

void checkGroup(const QMap<QString, QVariant> &values,
                const QStringList &fields,
                double minimum,
                double maximum,
                const QString &unit,
                ControlAlgorithm::SensorCheck &result)
{
    for (const QString &field : fields) {
        const double value = engineeringValue(values.value(field));
        if (outside(value, minimum, maximum)) {
            result.faultyFields.append(field);
            result.message = QString::fromUtf8("%1读数 %2 %3 超出有效范围 %4～%5 %3")
                .arg(sensorLabel(field))
                .arg(value, 0, 'f', 1)
                .arg(unit)
                .arg(minimum, 0, 'f', 1)
                .arg(maximum, 0, 'f', 1);
            result.state = ControlAlgorithm::SensorCheck::Fault;
            return;
        }
    }
}

double bounded(double value, double minimum, double maximum)
{
    return qMax(minimum, qMin(maximum, value));
}

} // namespace

namespace ControlAlgorithm {

SensorCheck checkTemperatureHumidity(
    const QMap<QString, QVariant> &values,
    const SensorLimits &limits)
{
    SensorCheck result;
    const QStringList temperatureFields = {
        "th1_temp", "th2_temp", "th3_temp"
    };
    const QStringList humidityFields = {
        "th1_humi", "th2_humi", "th3_humi"
    };
    QStringList missing;
    for (const QString &field : temperatureFields + humidityFields) {
        if (!values.contains(field))
            missing.append(sensorLabel(field));
    }
    result.state = SensorCheck::Healthy;
    result.message = QString::fromUtf8("温湿度自检正常");
    checkGroup(values, temperatureFields,
               limits.temperatureMin, limits.temperatureMax,
               QString::fromUtf8("℃"), result);
    if (result.state == SensorCheck::Fault)
        return result;
    checkGroup(values, humidityFields,
               limits.humidityMin, limits.humidityMax,
               QString::fromUtf8("%RH"), result);
    if (result.state == SensorCheck::Fault)
        return result;
    if (!missing.isEmpty()) {
        result.state = SensorCheck::Waiting;
        result.faultyFields = missing;
        result.message = QString::fromUtf8("等待温湿度自检数据");
    }
    return result;
}

ControlOutput thresholdControl(double temperature,
                               double target,
                               const ThresholdConfig &config,
                               int currentOt3,
                               int currentOt4)
{
    const int ot3 = currentOt3 != 0 ? 1 : 0;
    const int ot4 = currentOt4 != 0 ? 1 : 0;
    int currentStage = 0;
    if (ot3 && ot4) {
        currentStage = 3;
    } else if ((config.firstStageOt3 && ot3)
               || (!config.firstStageOt3 && ot4)) {
        currentStage = 1;
    } else if (ot3 || ot4) {
        currentStage = 2;
    }

    const double error = target - temperature;
    const double boundaries[] = {
        config.singleStageDelta,
        config.secondStageDelta,
        config.dualStageDelta
    };
    int desiredStage = currentStage;
    while (desiredStage < 3
           && error + kThresholdComparisonEpsilon
               >= boundaries[desiredStage] + config.hysteresis)
        ++desiredStage;
    while (desiredStage > 0
           && error - kThresholdComparisonEpsilon
               <= boundaries[desiredStage - 1] - config.hysteresis)
        --desiredStage;

    ControlOutput output;
    output.hasCommand = desiredStage != currentStage;
    output.demandPercent = desiredStage * 100.0 / 3.0;
    if (desiredStage == 3) {
        output.ot3 = 1;
        output.ot4 = 1;
    } else if (desiredStage == 2) {
        output.ot3 = config.firstStageOt3 ? 0 : 1;
        output.ot4 = config.firstStageOt3 ? 1 : 0;
    } else if (desiredStage == 1) {
        output.ot3 = config.firstStageOt3 ? 1 : 0;
        output.ot4 = config.firstStageOt3 ? 0 : 1;
    }
    return output;
}

double dewPointCelsius(double temperature, double relativeHumidity)
{
    if (!std::isfinite(temperature) || !std::isfinite(relativeHumidity)
        || relativeHumidity <= 0.0)
        return -std::numeric_limits<double>::infinity();
    const double humidity = bounded(relativeHumidity, 0.000001, 100.0);
    const double gamma = std::log(humidity / 100.0)
        + 17.62 * temperature / (243.12 + temperature);
    return 243.12 * gamma / (17.62 - gamma);
}

int condensationHeatingStage(double surfaceTemperature,
                             double dewPoint,
                             const CondensationConfig &config,
                             int currentStage)
{
    if (!std::isfinite(surfaceTemperature) || !std::isfinite(dewPoint))
        return 0;
    currentStage = qBound(0, currentStage, 3);
    const double margin = surfaceTemperature - dewPoint;
    const double boundaries[] = {
        config.singleStageMargin,
        config.secondStageMargin,
        config.dualStageMargin
    };
    int desiredStage = currentStage;
    while (desiredStage < 3
           && margin - kThresholdComparisonEpsilon
               <= boundaries[desiredStage] - config.hysteresis)
        ++desiredStage;
    while (desiredStage > 0
           && margin + kThresholdComparisonEpsilon
               >= boundaries[desiredStage - 1] + config.hysteresis)
        --desiredStage;
    return desiredStage;
}

ControlOutput pidControl(double temperature,
                         double target,
                         qint64 nowMs,
                         const PidConfig &config,
                         PidState &state)
{
    const double error = target - temperature;
    double dt = 1.0;
    double derivative = 0.0;
    if (state.initialized && nowMs > state.lastUpdateMs) {
        dt = bounded((nowMs - state.lastUpdateMs) / 1000.0, 0.1, 10.0);
        derivative = (error - state.lastError) / dt;
    }

    const double integralLimit = config.ki > 0.000001
        ? 100.0 / config.ki : 0.0;
    double candidateIntegral = state.integral;
    if (config.ki > 0.000001)
        candidateIntegral = bounded(state.integral + error * dt,
                                    -integralLimit, integralLimit);

    const double candidate = config.kp * error
        + config.ki * candidateIntegral + config.kd * derivative;
    const bool canIntegrate = (candidate >= 0.0 && candidate <= 100.0)
        || (candidate < 0.0 && error > 0.0)
        || (candidate > 100.0 && error < 0.0);
    if (canIntegrate)
        state.integral = candidateIntegral;

    const double demand = bounded(config.kp * error
        + config.ki * state.integral + config.kd * derivative, 0.0, 100.0);
    state.lastError = error;
    state.lastUpdateMs = nowMs;
    state.initialized = true;

    ControlOutput output;
    output.hasCommand = true;
    output.demandPercent = demand;
    if (demand >= config.dualStagePercent) {
        output.ot3 = 1;
        output.ot4 = 1;
    } else if (demand >= config.secondStagePercent) {
        output.ot3 = config.firstStageOt3 ? 0 : 1;
        output.ot4 = config.firstStageOt3 ? 1 : 0;
    } else if (demand >= config.singleStagePercent) {
        output.ot3 = config.firstStageOt3 ? 1 : 0;
        output.ot4 = config.firstStageOt3 ? 0 : 1;
    }
    return output;
}

} // namespace ControlAlgorithm
