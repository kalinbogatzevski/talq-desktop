#pragma once

#include <QObject>
#include <QString>

namespace TalQUpdates {
    constexpr auto kManifestUrl   = "https://example.invalid/public.php/webdav/talq-latest.json";
    constexpr auto kAssetBaseUrl  = "https://example.invalid/public.php/webdav/";
    constexpr auto kShareToken    = "REDACTED";
    constexpr auto kSharePassword = "REDACTED";
}

class AppSettings : public QObject
{
    Q_OBJECT

public:
    explicit AppSettings(QObject *parent = nullptr);

    Q_INVOKABLE bool isAutoStart() const;
    Q_INVOKABLE void setAutoStart(bool enabled);
};
