#include "UpdateChecker.h"

#include "AppSettings.h"
#include "VersionCompare.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QDebug>
#include <QVector>

UpdateChecker::UpdateChecker(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent), m_nam(nam)
{
    // 5-min poll — the manifest fetch is a single sub-1KB JSON GET, so CPU
    // and bandwidth cost is negligible. Short interval matters during active
    // development (rapid 0.25.x point releases) so testers see fresh builds
    // without restarting the app.
    m_pollTimer.setInterval(5 * 60 * 1000);
    connect(&m_pollTimer, &QTimer::timeout, this, &UpdateChecker::checkNow);
}

void UpdateChecker::start()
{
    if (!TalQUpdates::kEnabled) return;   // no updater in the generic/OSS build
    if (!autoCheckEnabled()) return;
    // First check shortly after launch (the network stack and main window are
    // up by ~3 s; the manifest GET is a sub-1KB JSON on the network thread so
    // it never competes with the UI). Was 30 s, which felt unresponsive.
    QTimer::singleShot(3 * 1000, this, &UpdateChecker::checkNow);
    m_pollTimer.start();
}

void UpdateChecker::stop()
{
    m_pollTimer.stop();
}

bool UpdateChecker::autoCheckEnabled() const
{
    return QSettings().value(QStringLiteral("updates/autoCheck"), true).toBool();
}

void UpdateChecker::setAutoCheckEnabled(bool on)
{
    QSettings().setValue(QStringLiteral("updates/autoCheck"), on);
    if (on) start(); else stop();
}

bool UpdateChecker::betaChannelEnabled() const
{
    return QSettings().value(QStringLiteral("updates/betaChannel"), false).toBool();
}

void UpdateChecker::setBetaChannelEnabled(bool on)
{
    QSettings().setValue(QStringLiteral("updates/betaChannel"), on);
    // Re-check immediately so switching channel takes effect now, not at
    // the next 5-min poll.
    if (autoCheckEnabled()) checkNow();
}

void UpdateChecker::checkNow()
{
    if (!TalQUpdates::kEnabled) return;   // OSS build: no 123NET update endpoint
    m_betaAttempt = betaChannelEnabled();
    fetchManifest();
}

void UpdateChecker::acceptUpdate()
{
    if (!m_hasPendingUpdate) return;
    startDownload();
}

void UpdateChecker::deferUpdate()
{
    m_hasPendingUpdate = false;
    QTimer::singleShot(60 * 60 * 1000, this, &UpdateChecker::checkNow);
}

QString UpdateChecker::brandKeyForThisBuild()
{
#ifdef TALQ_BRAND_123NET
    return QStringLiteral("123net");
#else
    return QStringLiteral("generic");
#endif
}

bool UpdateChecker::versionNewer(const QString &candidate, const QString &current)
{
    // Delegate to the header-only helper so this exact logic is unit-tested
    // without a Qt runtime (see tests/version_compare_test.cpp).
    return talq::versionNewer(candidate.toStdString(), current.toStdString());
}

void UpdateChecker::fetchManifest()
{
    if (!m_nam) return;
    QNetworkRequest req;
    if (TalQUpdates::kUseGithub) {
        // GitHub: /releases/latest excludes prereleases & drafts; /releases
        // returns ALL releases newest-first INCLUDING prereleases. Beta =
        // the list endpoint, take the newest non-draft (see onManifestFetched).
        QString api = QString::fromLatin1(TalQUpdates::kGithubApi);
        if (m_betaAttempt)
            api.replace(QStringLiteral("/releases/latest"),
                        QStringLiteral("/releases?per_page=20"));
        req.setUrl(QUrl(api));
        req.setRawHeader("Accept", "application/vnd.github+json");
        req.setRawHeader("User-Agent", "TalQ-UpdateChecker");
    } else {
        // Branded ncloud: beta manifest is the talq-beta-latest.json
        // sibling of the stable talq-latest.json. A missing one falls back
        // to stable in onManifestFetched.
        QString manifestUrl = QString::fromLatin1(TalQUpdates::kManifestUrl);
        if (m_betaAttempt)
            manifestUrl.replace(QStringLiteral("talq-latest.json"),
                                QStringLiteral("talq-beta-latest.json"));
        req.setUrl(QUrl(manifestUrl));
        QString creds = QStringLiteral("%1:%2")
                            .arg(QString::fromLatin1(TalQUpdates::kShareToken),
                                 QString::fromLatin1(TalQUpdates::kSharePassword));
        req.setRawHeader("Authorization",
                         "Basic " + creds.toUtf8().toBase64());
    }
    req.setTransferTimeout(30 * 1000);

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onManifestFetched(reply);
    });
}

void UpdateChecker::onManifestFetched(QNetworkReply *reply)
{
    reply->deleteLater();

    // A beta attempt that fails (network error, not found, unparseable, or
    // missing fields) transparently degrades to the stable channel exactly
    // once — beta is opt-in convenience, never a way to get stuck.
    auto tryStableFallback = [this]() -> bool {
        if (!m_betaAttempt) return false;
        qInfo() << "UpdateChecker: beta channel unavailable — using stable";
        m_betaAttempt = false;
        fetchManifest();
        return true;
    };

    if (reply->error() != QNetworkReply::NoError) {
        if (tryStableFallback()) return;
        qWarning() << "UpdateChecker: manifest fetch failed" << reply->errorString();
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());

    QJsonObject root;
    if (TalQUpdates::kUseGithub && m_betaAttempt && doc.isArray()) {
        // GitHub /releases is newest-first; take the newest non-draft
        // (prereleases included — that is the beta).
        for (const QJsonValue &rv : doc.array()) {
            const QJsonObject o = rv.toObject();
            if (o.value(QStringLiteral("draft")).toBool()) continue;
            root = o;
            break;
        }
        if (root.isEmpty()) {
            if (tryStableFallback()) return;
            qWarning() << "UpdateChecker: no usable release in /releases list";
            return;
        }
    } else if (doc.isObject()) {
        root = doc.object();
    } else {
        if (tryStableFallback()) return;
        qWarning() << "UpdateChecker: manifest not a JSON object";
        return;
    }

    Manifest m;
    if (TalQUpdates::kUseGithub) {
        // GitHub Releases API response shape.
        QString tag = root.value(QStringLiteral("tag_name")).toString();
        if (tag.startsWith(QLatin1Char('v')) || tag.startsWith(QLatin1Char('V')))
            tag.remove(0, 1);
        m.version     = tag;
        m.releaseDate = root.value(QStringLiteral("published_at")).toString();
        m.notes       = root.value(QStringLiteral("body")).toString();
        // The OSS release publishes the generic installer as
        // TalQ-v<ver>-Setup.exe; pick that asset.
        const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue &av : assets) {
            const QJsonObject a = av.toObject();
            const QString name = a.value(QStringLiteral("name")).toString();
            if (name.startsWith(QStringLiteral("TalQ-v"))
                && name.endsWith(QStringLiteral("-Setup.exe"))) {
                m.assetFilename = name;
                m.assetUrl = a.value(QStringLiteral("browser_download_url")).toString();
                break;
            }
        }
        m.assetSha256.clear();   // GitHub exposes no per-asset digest
    } else {
        m.version     = root.value(QStringLiteral("version")).toString();
        m.releaseDate = root.value(QStringLiteral("releaseDate")).toString();
        m.notes       = root.value(QStringLiteral("notes")).toString();

        QString brand = brandKeyForThisBuild();
        m.assetFilename = root.value(QStringLiteral("assets")).toObject()
                              .value(brand).toString();
        m.assetSha256   = root.value(QStringLiteral("sha256")).toObject()
                              .value(brand).toString();
        if (!m.assetFilename.isEmpty())
            m.assetUrl = QString::fromLatin1(TalQUpdates::kAssetBaseUrl)
                         + m.assetFilename;
    }

    if (m.version.isEmpty() || m.assetUrl.isEmpty()) {
        qWarning() << "UpdateChecker: manifest missing version or asset url";
        return;
    }

    // Prerelease flag: true when the manifest came from the beta channel
    // OR when the GitHub release object explicitly says prerelease. The
    // banner UI uses this to add a "PRE-RELEASE" chip so beta testers
    // can tell at a glance what they're about to install.
    m.prerelease = m_betaAttempt
                || root.value(QStringLiteral("prerelease")).toBool();

    const QString currentVersion = QStringLiteral(TALQ_VERSION);
    if (versionNewer(m.version, currentVersion)) {
        m_lastManifest = m;
        m_hasPendingUpdate = true;
        emit updateAvailable(m);
    }
}
void UpdateChecker::startDownload()
{
    const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_downloadPath = tmp + QStringLiteral("/talq-update.exe");

    auto *out = new QFile(m_downloadPath);
    if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadFailed(QStringLiteral("Cannot open temp file: %1")
                                .arg(out->errorString()));
        out->deleteLater();
        return;
    }

    QNetworkRequest req{QUrl(m_lastManifest.assetUrl)};
    // GitHub redirects release-asset downloads to its CDN; follow safely.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "TalQ-UpdateChecker");
    if (!TalQUpdates::kUseGithub) {
        QString creds = QStringLiteral("%1:%2")
                            .arg(QString::fromLatin1(TalQUpdates::kShareToken),
                                 QString::fromLatin1(TalQUpdates::kSharePassword));
        req.setRawHeader("Authorization",
                         "Basic " + creds.toUtf8().toBase64());
    }
    req.setTransferTimeout(10 * 60 * 1000);

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::readyRead, this, [reply, out]() {
        out->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress,
            this, &UpdateChecker::onDownloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, out]() {
        onDownloadFinished(reply, out);
    });
}

void UpdateChecker::onDownloadProgress(qint64 received, qint64 total)
{
    if (total <= 0) return;
    emit downloadProgress(100.0 * double(received) / double(total));
}

void UpdateChecker::onDownloadFinished(QNetworkReply *reply, QFile *out)
{
    out->write(reply->readAll());
    out->flush();
    out->close();

    const bool net_ok = (reply->error() == QNetworkReply::NoError);
    const QString path = m_downloadPath;
    const QString netErr = reply->errorString();
    out->deleteLater();
    reply->deleteLater();

    if (!net_ok) {
        QFile::remove(path);
        emit downloadFailed(netErr);
        return;
    }

    if (!m_lastManifest.assetSha256.isEmpty()) {
        if (!verifySha256(path, m_lastManifest.assetSha256)) {
            QFile::remove(path);
            emit downloadFailed(QStringLiteral("Checksum verification failed"));
            return;
        }
    } else {
        // GitHub provides no per-asset digest; integrity rests on the
        // HTTPS transport to github.com and its CDN.
        qInfo() << "UpdateChecker: no checksum in release; relying on HTTPS";
    }

    emit readyToLaunch(path);
}

bool UpdateChecker::verifySha256(const QString &filePath, const QString &expectedHex)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash h(QCryptographicHash::Sha256);
    constexpr qint64 chunk = 64 * 1024;
    QByteArray buf;
    while (!f.atEnd()) {
        buf = f.read(chunk);
        h.addData(buf);
    }
    const QString got = QString::fromLatin1(h.result().toHex()).toLower();
    return got == expectedHex.toLower();
}
