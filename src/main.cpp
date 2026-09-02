#include "appui.h"
#include <QApplication>
#include <QFileInfo>
#include <QDir>

static QString locateConfigFile()
{
    const QStringList candidates = {
        QDir::currentPath() + "/config/app.ini",
        QDir::currentPath() + "/app.ini",
        QApplication::applicationDirPath() + "/config/app.ini",
        QApplication::applicationDirPath() + "/app.ini",
        "D:/RS485Control/config/app.ini"
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return path;
    }
    return candidates.last();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("RS485Control");
    app.setOrganizationName("RS485Control");

    // 注册跨线程信号槽所需的自定义类型
    qRegisterMetaType<DeviceProfile::DeviceKey>("DeviceProfile::DeviceKey");
    qRegisterMetaType<SerialPortWorker::PollTask>("SerialPortWorker::PollTask");
    qRegisterMetaType<SerialPortWorker::WriteTask>("SerialPortWorker::WriteTask");
    qRegisterMetaType<QVector<SerialPortWorker::PollTask>>(
        "QVector<SerialPortWorker::PollTask>");

    const QString configPath = locateConfigFile();
    if (!AppConfig::instance().load(configPath)) {
        // 配置文件不存在时仍启动，便于首次运行
    }

    MainWindow w;
    w.showMaximized();

    return app.exec();
}
