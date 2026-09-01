#include "controlalgorithm.h"

#include <QtGlobal>
#include <QVector>
#include <cmath>

namespace {

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
    const SensorLimits &limits,
    bool gracePeriodExpired)
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
    if (!missing.isEmpty()) {
        result.state = gracePeriodExpired ? SensorCheck::Fault : SensorCheck::Waiting;
        result.faultyFields = missing;
        result.message = gracePeriodExpired
            ? QString::fromUtf8("自检超时，缺少：%1").arg(missing.join(QString::fromUtf8("、")))
            : QString::fromUtf8("等待温湿度自检数据");
        return result;
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
    return result;
}

ControlOutput thresholdControl(double temperature,
                               double target,
                               double lowerHysteresis,
                               double upperHysteresis,
                               int currentOt3,
                               int currentOt4)
{
    ControlOutput output;
    output.ot3 = currentOt3 != 0 ? 1 : 0;
    output.ot4 = currentOt4 != 0 ? 1 : 0;
    if (temperature < target - lowerHysteresis) {
        output.hasCommand = true;
        output.ot3 = 1;
        output.ot4 = 1;
        output.demandPercent = 100.0;
    } else if (temperature > target + upperHysteresis) {
        output.hasCommand = true;
        output.ot3 = 0;
        output.ot4 = 0;
        output.demandPercent = 0.0;
    } else if (temperature > target) {
        output.hasCommand = true;
        output.ot3 = 1;
        output.ot4 = 0;
        output.demandPercent = 50.0;
    }
    return output;
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
    } else if (demand >= config.singleStagePercent) {
        output.ot3 = 1;
        output.ot4 = 0;
    }
    return output;
}

} // namespace ControlAlgorithm
