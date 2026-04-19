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
        QString assetSha256;
    };

    explicit UpdateChecker(QNetworkAccessManager *nam, QObject *parent = nullptr);

    void start();
    void stop();

    bool autoCheckEnabled() const;
    void setAutoCheckEnabled(bool on);

signals:
    void updateAvailable(const UpdateChecker::Manifest &m);
    void downloadProgress(qreal percent);
    void downloadFailed(const QString &reason);
    void readyToLaunch(const QString &installerPath);

public slots:
    void checkNow();
    void acceptUpdate();
    void deferUpdate();

private:
    void fetchManifest();
    void onManifestFetched(QNetworkReply *reply);
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
};

Q_DECLARE_METATYPE(UpdateChecker::Manifest)
