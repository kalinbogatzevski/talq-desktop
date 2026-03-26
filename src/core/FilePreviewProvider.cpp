#include "core/FilePreviewProvider.h"
#include <QQuickTextureFactory>
#include <QNetworkReply>
#include <QCoreApplication>
#include <QDebug>

// ─── Response ───

FilePreviewResponse::FilePreviewResponse(int fileId, const QSize &requestedSize,
                                         ApiClient *api, QHash<int, QImage> &cache,
                                         FilePreviewProvider *provider)
    : m_fileId(fileId), m_requestedSize(requestedSize), m_api(api), m_cache(cache), m_provider(provider)
{
    // Move to main thread — requestImageResponse is called from the render thread,
    // but QNetworkAccessManager lives on the main thread.
    moveToThread(QCoreApplication::instance()->thread());

    // Check memory cache (read-only, safe from any thread)
    if (cache.contains(fileId)) {
        m_image = cache[fileId];
        // Defer finished — caller connects after constructor returns
        QMetaObject::invokeMethod(this, &FilePreviewResponse::finished, Qt::QueuedConnection);
        return;
    }

    // Defer network fetch to main thread
    QMetaObject::invokeMethod(this, &FilePreviewResponse::doFetch, Qt::QueuedConnection);
}

void FilePreviewResponse::doFetch()
{
    int w = m_requestedSize.width() > 0 ? qMax(m_requestedSize.width(), 1024) : 1024;
    int h = qMax(w * 3 / 4, 768);

    QString path = QString("/index.php/core/preview?fileId=%1&x=%2&y=%3&a=1&forceIcon=0&mode=cover")
        .arg(m_fileId).arg(w).arg(h);

    auto *reply = m_api->getAbsoluteUrl(path);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QImage img;
            if (img.loadFromData(reply->readAll())) {
                m_image = img;
                // Guard against duplicate fileId overwrites
                if (m_cache.contains(m_fileId)) {
                    m_provider->m_cachedBytes -= m_cache[m_fileId].sizeInBytes();
                } else {
                    m_provider->m_cacheOrder.append(m_fileId);
                }
                m_cache[m_fileId] = img;
                m_provider->m_cachedBytes += img.sizeInBytes();
                m_provider->evictIfNeeded();
            }
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
    return new FilePreviewResponse(fileId, requestedSize, m_api, m_cache, this);
}

qint64 FilePreviewProvider::cacheBytes() const
{
    return m_cachedBytes;
}

void FilePreviewProvider::evictIfNeeded()
{
    while (m_cachedBytes > MAX_CACHE_BYTES && !m_cacheOrder.isEmpty()) {
        int evictId = m_cacheOrder.takeFirst();
        auto it = m_cache.find(evictId);
        if (it != m_cache.end()) {
            m_cachedBytes -= it.value().sizeInBytes();
            m_cache.erase(it);
        }
    }
}
