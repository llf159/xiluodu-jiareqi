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
    const SensorLimits &limits);

struct ControlOutput {
    bool hasCommand = false;
    int ot3 = 0;
    int ot4 = 0;
    double demandPercent = 0.0;
};

struct ThresholdConfig {
    double singleStageDelta = 0.3;
    double secondStageDelta = 1.5;
    double dualStageDelta = 3.0;
    double hysteresis = 0.2;
    bool firstStageOt3 = true;
};

ControlOutput thresholdControl(double temperature,
                               double target,
                               const ThresholdConfig &config,
                               int currentOt3,
                               int currentOt4);

struct CondensationConfig {
    double singleStageMargin = 3.0;
    double secondStageMargin = 2.0;
    double dualStageMargin = 1.0;
    double hysteresis = 0.2;
};

double dewPointCelsius(double temperature, double relativeHumidity);
int condensationHeatingStage(double surfaceTemperature,
                             double dewPoint,
                             const CondensationConfig &config,
                             int currentStage);

struct PidConfig {
    double kp = 12.0;
    double ki = 0.15;
    double kd = 0.0;
    double singleStagePercent = 10.0;
    double secondStagePercent = 35.0;
    double dualStagePercent = 60.0;
    bool firstStageOt3 = true;
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
