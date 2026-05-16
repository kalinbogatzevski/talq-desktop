#pragma once

#include <QObject>
#include <QString>

// The in-app auto-updater has two compile-time channels. The branded
// distribution uses a private channel (endpoint + credentials supplied
// from the private build store). The generic / open-source build uses
// public GitHub Releases (kUseGithub=true): the GitHub API and repo are
// public, so there are no credentials and no private infrastructure in
// this source — only the public api.github.com endpoint.
namespace TalQUpdates {
#ifdef TALQ_BRAND_123NET
    // The real endpoint + public-share credentials are NOT in the public
    // source. They live in the private branding store
    // (private/branding/123net/brand_updates.inc), put on the include path
    // only for the 123NET build by CMake. A branded build without the
    // private store fails to compile — that's the intended contract.
    #include "brand_updates.inc"
    constexpr bool kUseGithub     = false;   // branded: private channel
    constexpr auto kGithubApi     = "";
#else
    // Open-source build: auto-update from public GitHub Releases. No
    // credentials, no private infrastructure — the GitHub API and the
    // public repo are not sensitive.
    constexpr bool kEnabled       = true;
    constexpr bool kUseGithub     = true;
    constexpr auto kGithubApi     =
        "https://api.github.com/repos/kalinbogatzevski/talq-desktop/releases/latest";
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
