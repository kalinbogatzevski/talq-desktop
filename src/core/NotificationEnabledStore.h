#pragma once

// NotificationEnabledStore -- the persisted "Desktop notifications" switch: group "Notifications",
// key "enabled", default ON. Qt Core only. NotificationManager reads it once in its constructor and
// writes it from setNotificationsEnabled, which is the ONLY writer (the Settings checkbox and the
// tray action both go through that setter). See tests/notification_enabled_store_test.cpp.
//
// Before this header the key was written by the Settings handler only and read back only to set
// that checkbox, so a switch turned off in Settings was honoured for that session alone; the tray
// action persisted nothing at all.
//
// load() writes nothing: QSettings::value() with a default does not create the key, and the group
// is left closed so a caller's own beginGroup/endGroup pairing is never disturbed.

#include <QSettings>
#include <QString>

namespace talq {

inline bool loadNotificationsEnabled(QSettings &s)
{
    s.beginGroup(QStringLiteral("Notifications"));
    const bool on = s.value(QStringLiteral("enabled"), true).toBool();
    s.endGroup();
    return on;
}

inline void saveNotificationsEnabled(QSettings &s, bool on)
{
    s.beginGroup(QStringLiteral("Notifications"));
    s.setValue(QStringLiteral("enabled"), on);
    s.endGroup();
}

} // namespace talq
