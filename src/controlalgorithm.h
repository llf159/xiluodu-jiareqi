#ifndef CONTROLALGORITHM_H
#define CONTROLALGORITHM_H

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace ControlAlgorithm {

struct SensorLimits {
    double temperatureMin = -40.0;
    double temperatureMax = 85.0;
    double humidityMin = 0.0;
    double humidityMax = 100.0;
    double temperatureMaxDeviation = 15.0;
    double humidityMaxDeviation = 30.0;
};

struct SensorCheck {
    enum State {
        Waiting,
        Healthy,
        Fault
    };

    State state = Waiting;
    QString message;
    QStringList faultyFields;
};

SensorCheck checkTemperatureHumidity(
    const QMap<QString, QVariant> &values,
    const SensorLimits &limits,
    bool gracePeriodExpired);

struct ControlOutput {
    bool hasCommand = false;
    int ot3 = 0;
    int ot4 = 0;
    double demandPercent = 0.0;
};

ControlOutput thresholdControl(double temperature,
                               double target,
                               double lowerHysteresis,
                               double upperHysteresis,
                               int currentOt3,
                               int currentOt4);

struct PidConfig {
    double kp = 12.0;
    double ki = 0.15;
    double kd = 0.0;
    double singleStagePercent = 10.0;
    double dualStagePercent = 60.0;
};

struct PidState {
    double integral = 0.0;
    double lastError = 0.0;
    qint64 lastUpdateMs = 0;
    bool initialized = false;
};

ControlOutput pidControl(double temperature,
                         double target,
                         qint64 nowMs,
                         const PidConfig &config,
                         PidState &state);

} // namespace ControlAlgorithm

#endif // CONTROLALGORITHM_H
