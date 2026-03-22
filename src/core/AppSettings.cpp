#include "core/AppSettings.h"
#include <QCoreApplication>
#include <QSettings>
#include <QDebug>

static const char *kAutoRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
{
}

bool AppSettings::isAutoStart() const
{
    QSettings reg(kAutoRunKey, QSettings::NativeFormat);
    return reg.contains(QCoreApplication::applicationName());
}

void AppSettings::setAutoStart(bool enabled)
{
    QSettings reg(kAutoRunKey, QSettings::NativeFormat);
    QString appName = QCoreApplication::applicationName();

    if (enabled) {
        QString path = QCoreApplication::applicationFilePath().replace('/', '\\');
        reg.setValue(appName, "\"" + path + "\"");
        qDebug() << "AppSettings: auto-start enabled:" << path;
    } else {
        reg.remove(appName);
        qDebug() << "AppSettings: auto-start disabled";
    }
}
