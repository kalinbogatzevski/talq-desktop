#include "core/AppSettings.h"
#include <QCoreApplication>
#include <QSettings>
#include <QDebug>

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
{
}

bool AppSettings::isAutoStart() const
{
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    return reg.contains(QCoreApplication::applicationName());
}

void AppSettings::setAutoStart(bool enabled)
{
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (enabled) {
        QString path = QCoreApplication::applicationFilePath().replace('/', '\\');
        reg.setValue(QCoreApplication::applicationName(), "\"" + path + "\"");
        qDebug() << "AppSettings: auto-start enabled:" << path;
    } else {
        reg.remove(QCoreApplication::applicationName());
        qDebug() << "AppSettings: auto-start disabled";
    }
}
