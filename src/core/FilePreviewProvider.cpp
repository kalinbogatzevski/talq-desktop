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

    // Fetch from server — request wide rectangle to avoid square crop
    int w = requestedSize.width() > 0 ? qMax(requestedSize.width(), 1024) : 1024;
    int h = qMax(w * 3 / 4, 768);  // wide aspect to avoid cropping landscape images
    qDebug() << "FilePreview: fetching fileId" << fileId << "at" << w << "x" << h << "(requested:" << requestedSize << ")";

    // a=1 preserves aspect ratio, forceIcon=0 gets actual preview
    QString path = QString("/index.php/core/preview?fileId=%1&x=%2&y=%3&a=1&forceIcon=0&mode=cover")
        .arg(fileId).arg(w).arg(h);

    auto *reply = api->getAbsoluteUrl(path);

    connect(reply, &QNetworkReply::finished, this, [this, reply, fileId, &cache]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QImage img;
            if (img.loadFromData(reply->readAll())) {
                m_image = img;
                cache[fileId] = img;
                qDebug() << "FilePreview: loaded fileId" << fileId << "size:" << img.size();
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

qint64 FilePreviewProvider::cacheBytes() const
{
    qint64 total = 0;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        total += it.value().sizeInBytes();
    return total;
}
