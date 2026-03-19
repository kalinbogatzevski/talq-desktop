#include "core/FilePreviewProvider.h"
#include <QQuickTextureFactory>
#include <QNetworkReply>
#include <QDebug>

// ─── Response ───

FilePreviewResponse::FilePreviewResponse(int fileId, const QSize &requestedSize,
                                         ApiClient *api, QHash<int, QImage> &cache)
{
    // Check memory cache
    if (cache.contains(fileId)) {
        m_image = cache[fileId];
        emit finished();
        return;
    }

    // Fetch from server
    int w = requestedSize.width() > 0 ? requestedSize.width() : 400;
    int h = requestedSize.height() > 0 ? requestedSize.height() : 400;

    QString path = QString("/index.php/core/preview?fileId=%1&x=%2&y=%3")
        .arg(fileId).arg(w).arg(h);

    auto *reply = api->getAbsoluteUrl(path);

    connect(reply, &QNetworkReply::finished, this, [this, reply, fileId, &cache]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QImage img;
            if (img.loadFromData(reply->readAll())) {
                m_image = img;
                cache[fileId] = img;
            }
        } else {
            qDebug() << "Preview fetch failed for fileId" << fileId << reply->errorString();
        }
        emit finished();
    });
}

QQuickTextureFactory *FilePreviewResponse::textureFactory() const
{
    return QQuickTextureFactory::textureFactoryForImage(m_image);
}

// ─── Provider ───

FilePreviewProvider::FilePreviewProvider(ApiClient *api)
    : m_api(api)
{
}

QQuickImageResponse *FilePreviewProvider::requestImageResponse(
    const QString &id, const QSize &requestedSize)
{
    int fileId = id.toInt();
    return new FilePreviewResponse(fileId, requestedSize, m_api, m_cache);
}
