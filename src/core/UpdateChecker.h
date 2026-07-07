#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    struct Manifest {
        QString version;
        QString releaseDate;
        QString notes;
        QString assetFilename;
        QString assetUrl;       // absolute download URL (GitHub or ncloud)
        QString assetSha256;    // may be empty (GitHub provides no digest)
        bool prerelease = false;  // true when the manifest came from the
                                  // beta channel (so the banner can say so)
        // #31 forced-downgrade: when the channel manifest sets "pin": true, the
        // updater installs this EXACT version even if it is LOWER than the one
        // installed (a controlled rollback) — the escape hatch for a mis-publish
        // that auto-shipped a too-high version. Without the flag a lower version
        // is never offered. true only via the explicit manifest flag.
        bool pin = false;
    };

    explicit UpdateChecker(QNetworkAccessManager *nam, QObject *parent = nullptr);

    void start();
    void stop();

    bool autoCheckEnabled() const;
    void setAutoCheckEnabled(bool on);

    // Opt-in pre-release (beta) channel. Generic build: query GitHub
    // /releases (newest incl. prereleases) instead of /releases/latest.
    // Branded build: try talq-beta-latest.json, fall back to the stable
    // manifest. Off by default; persisted in QSettings updates/betaChannel.
    bool betaChannelEnabled() const;
    void setBetaChannelEnabled(bool on);

signals:
    void updateAvailable(const UpdateChecker::Manifest &m);
    void downloadProgress(qreal percent);
    void downloadFailed(const QString &reason);
    void readyToLaunch(const QString &installerPath);

public slots:
    void checkNow();
    void acceptUpdate();
    void deferUpdate();
    // Re-runs startDownload() for the currently-pending manifest, used
    // by MainWindow's self-heal path when QProcess::startDetached on
    // the just-downloaded installer fails (AV quarantine, stale lock,
    // truncated write). No-op if no manifest is pending.
    void retryDownload();

private:
    void fetchManifest();
    void onManifestFetched(QNetworkReply *reply);
    // GitHub exposes no per-asset digest field, so the release also
    // publishes a companion "<assetFilename>.sha256" text asset (plain hex,
    // no filename suffix) alongside the installer. If one is present in the
    // release's asset list, fetch it and populate m.assetSha256 before
    // finalizing -- gives the generic/GitHub channel the same
    // download-integrity check the branded/ncloud channel already has via
    // its manifest's native sha256 field. Falls back to no checksum (today's
    // behaviour) if the asset is absent or the fetch fails; never blocks an
    // update on this being unavailable.
    void fetchChecksumThenFinalize(Manifest m, const QString &checksumUrl);
    void finalizeManifest(Manifest m);
    void startDownload();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished(QNetworkReply *reply, QFile *outFile);
    bool verifySha256(const QString &filePath, const QString &expectedHex);
    static QString brandKeyForThisBuild();
    static bool versionNewer(const QString &candidate, const QString &current);

    QNetworkAccessManager *m_nam;
    QTimer m_pollTimer;
    Manifest m_lastManifest;
    bool m_hasPendingUpdate = false;
    QString m_downloadPath;
    // Digest snapshotted at startDownload() time, verified against in
    // onDownloadFinished. A ~75 MB install download takes minutes on a field
    // link; a periodic re-check landing mid-download can overwrite
    // m_lastManifest with a NEWER release's digest, which would then fail the
    // OLD (perfectly good) download's verification. Verify against what we
    // actually started downloading, not the live manifest.
    QString m_downloadExpectedSha256;
    // Monotonic id of the current check cycle. Bumped on every checkNow(); the
    // async companion-checksum continuation captures the id at fetch time and
    // no-ops if a newer cycle has since started, so a slow checksum reply
    // can't resurrect a stale/yanked release over a fresher one.
    quint64 m_checkGen = 0;
    // True while the in-flight fetch is the beta-channel attempt; cleared
    // before a stable fallback fetch so a missing/!valid beta manifest
    // transparently degrades to the stable channel (no infinite retry).
    bool m_betaAttempt = false;
    // In-flight asset fetch handles. Cleared by onDownloadFinished. Used
    // by startDownload to abort a still-running prior download (e.g.
    // self-heal retry called before the first reply finished) without
    // leaking a file handle and a network reply.
    QNetworkReply *m_currentReply = nullptr;
    QFile         *m_currentFile  = nullptr;
};

Q_DECLARE_METATYPE(UpdateChecker::Manifest)
