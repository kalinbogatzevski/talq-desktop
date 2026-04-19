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

void UpdateChecker::fetchManifest() { /* Task 2 */ }
void UpdateChecker::onManifestFetched(QNetworkReply *) { /* Task 2 */ }
void UpdateChecker::startDownload() { /* Task 3 */ }
void UpdateChecker::onDownloadProgress(qint64, qint64) { /* Task 3 */ }
void UpdateChecker::onDownloadFinished(QNetworkReply *, QFile *) { /* Task 3 */ }
bool UpdateChecker::verifySha256(const QString &, const QString &) { return true; /* Task 3 */ }
