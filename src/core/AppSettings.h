#pragma once

#include <QObject>
#include <QString>

// The in-app auto-updater is a 123NET-distribution feature: it points at
// 123NET's ncloud share (URL + public-share credentials). The generic /
// open-source build does NOT ship that infrastructure or those credentials
// and distributes via GitHub Releases instead, so the updater is compiled
// inert (kEnabled=false, empty endpoints) and never touches the network.
namespace TalQUpdates {
#ifdef TALQ_BRAND_123NET
    // The real endpoint + public-share credentials are NOT in the public
    // source. They live in the private branding store
    // (private/branding/123net/brand_updates.inc), put on the include path
    // only for the 123NET build by CMake. A branded build without the
    // private store fails to compile — that's the intended contract.
    #include "brand_updates.inc"
#else
    constexpr bool kEnabled       = false;
    constexpr auto kManifestUrl   = "";
    constexpr auto kAssetBaseUrl  = "";
    constexpr auto kShareToken    = "";
    constexpr auto kSharePassword = "";
#endif
}

class AppSettings : public QObject
{
    Q_OBJECT

public:
    explicit AppSettings(QObject *parent = nullptr);

    Q_INVOKABLE bool isAutoStart() const;
    Q_INVOKABLE void setAutoStart(bool enabled);
};
