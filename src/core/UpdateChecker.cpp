#include "UpdateChecker.h"

#include "AppSettings.h"

#include <QCryptographicHash>
#include <QFile>
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
    m_pollTimer.setInterval(4 * 60 * 60 * 1000);
    connect(&m_pollTimer, &QTimer::timeout, this, &UpdateChecker::checkNow);
}

void UpdateChecker::start()
{
    if (!autoCheckEnabled()) return;
    QTimer::singleShot(30 * 1000, this, &UpdateChecker::checkNow);
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

void UpdateChecker::checkNow()
{
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
    auto parts = [](const QString &v) {
        QVector<int> out;
        const QStringList segs = v.split(QLatin1Char('.'));
        for (const QString &s : segs) {
            bool ok = false;
            int n = s.toInt(&ok);
            out.push_back(ok ? n : 0);
        }
        return out;
    };
    auto a = parts(candidate);
    auto b = parts(current);
    int n = qMax(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        int va = i < a.size() ? a[i] : 0;
        int vb = i < b.size() ? b[i] : 0;
        if (va != vb) return va > vb;
    }
    return false;
}

void UpdateChecker::fetchManifest()
{
    if (!m_nam) return;
    QNetworkRequest req((QUrl(TalQUpdates::kManifestUrl)));
    QString creds = QStringLiteral("%1:%2")
                        .arg(QString::fromLatin1(TalQUpdates::kShareToken),
                             QString::fromLatin1(TalQUpdates::kSharePassword));
    req.setRawHeader("Authorization",
                     "Basic " + creds.toUtf8().toBase64());
    req.setTransferTimeout(30 * 1000);

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onManifestFetched(reply);
    });
}

void UpdateChecker::onManifestFetched(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "UpdateChecker: manifest fetch failed" << reply->errorString();
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        qWarning() << "UpdateChecker: manifest not a JSON object";
        return;
    }
    QJsonObject root = doc.object();

    Manifest m;
    m.version     = root.value(QStringLiteral("version")).toString();
    m.releaseDate = root.value(QStringLiteral("releaseDate")).toString();
    m.notes       = root.value(QStringLiteral("notes")).toString();

    QString brand = brandKeyForThisBuild();
    m.assetFilename = root.value(QStringLiteral("assets")).toObject()
                          .value(brand).toString();
    m.assetSha256   = root.value(QStringLiteral("sha256")).toObject()
                          .value(brand).toString();

    if (m.version.isEmpty() || m.assetFilename.isEmpty()) {
        qWarning() << "UpdateChecker: manifest missing version or asset. brand=" << brand;
        return;
    }

    const QString currentVersion = QStringLiteral(TALQ_VERSION);
    if (versionNewer(m.version, currentVersion)) {
        m_lastManifest = m;
        m_hasPendingUpdate = true;
        emit updateAvailable(m);
    }
}
void UpdateChecker::startDownload() { /* Task 3 */ }
void UpdateChecker::onDownloadProgress(qint64, qint64) { /* Task 3 */ }
void UpdateChecker::onDownloadFinished(QNetworkReply *, QFile *) { /* Task 3 */ }
bool UpdateChecker::verifySha256(const QString &, const QString &) { return true; /* Task 3 */ }
