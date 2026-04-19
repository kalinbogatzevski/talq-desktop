#pragma once

#include <QObject>
#include <QString>

namespace TalQUpdates {
    constexpr auto kManifestUrl   = "https://ncloud.123net.link/public.php/webdav/talq-latest.json";
    constexpr auto kAssetBaseUrl  = "https://ncloud.123net.link/public.php/webdav/";
    constexpr auto kShareToken    = "ezJnLiFtL6K3x4p";
    constexpr auto kSharePassword = "talq-public-updates";
}

class AppSettings : public QObject
{
    Q_OBJECT

public:
    explicit AppSettings(QObject *parent = nullptr);

    Q_INVOKABLE bool isAutoStart() const;
    Q_INVOKABLE void setAutoStart(bool enabled);
};
