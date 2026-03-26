#include "core/AvatarProvider.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QDebug>

// --- AvatarProvider ---

AvatarProvider::AvatarProvider(ApiClient *api)
    : m_api(api)
{
    m_cachePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/avatars";
    QDir().mkpath(m_cachePath);
}

QQuickImageResponse *AvatarProvider::requestImageResponse(
    const QString &id, const QSize &requestedSize)
{
    // Memory cache hit — instant, no I/O
    if (m_memCache.contains(id)) {
        return new AvatarCachedResponse(m_memCache[id]);
    }

    return new AvatarFetchResponse(id, requestedSize, m_api, m_cachePath, m_memCache, m_memCacheOrder);
}

// --- AvatarCachedResponse ---

AvatarCachedResponse::AvatarCachedResponse(const QImage &image)
    : m_image(image)
{
    // Defer — caller connects to finished() after constructor returns
    QMetaObject::invokeMethod(this, &AvatarCachedResponse::finished, Qt::QueuedConnection);
}

QQuickTextureFactory *AvatarCachedResponse::textureFactory() const
{
    return QQuickTextureFactory::textureFactoryForImage(m_image);
}

// --- AvatarFetchResponse ---

AvatarFetchResponse::AvatarFetchResponse(const QString &userId, const QSize &requestedSize,
                                         ApiClient *api, const QString &cachePath,
                                         QHash<QString, QImage> &memCache,
                                         QList<QString> &memCacheOrder)
    : m_userId(userId)
    , m_requestedSize(requestedSize)
    , m_api(api)
    , m_cachePath(cachePath)
    , m_memCache(memCache)
    , m_memCacheOrder(memCacheOrder)
{
    // Move to main thread — requestImageResponse is called from the render thread,
    // but QNetworkAccessManager and disk I/O must happen on the main thread.
    moveToThread(QCoreApplication::instance()->thread());
    QMetaObject::invokeMethod(this, &AvatarFetchResponse::loadFromDisk, Qt::QueuedConnection);
}

void AvatarFetchResponse::loadFromDisk()
{
    // Sanitize userId to prevent path traversal
    QString safeId = m_userId;
    safeId.remove(QRegularExpression("[/\\\\\\.]"));
    if (safeId.isEmpty()) safeId = "_unknown";
    QString filePath = m_cachePath + "/" + safeId + ".png";
    QFileInfo fi(filePath);

    if (fi.exists()) {
        if (fi.lastModified().secsTo(QDateTime::currentDateTime()) < 86400) {
            QImage img(filePath);
            if (!img.isNull()) {
                handleImage(img);
                return;
            }
        }
    }

    fetchFromServer();
}

void AvatarFetchResponse::fetchFromServer()
{
    if (!m_api || m_userId.isEmpty()) {
        emit finished();
        return;
    }

    // Room avatars need authenticated OCS API; user avatars are public
    QNetworkReply *reply;
    if (m_userId.startsWith("room/")) {
        QString token = m_userId.mid(5);  // strip "room/" prefix
        reply = m_api->getRaw("apps/spreed/api/v1/room/" + token + "/avatar");
    } else {
        reply = m_api->getAbsoluteUrl("/index.php/avatar/" + m_userId + "/64");
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        // Remove from pending replies to avoid dangling pointer
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            // Cache an empty image so we don't retry failed avatars
            m_memCache[m_userId] = QImage();
            emit finished();
            return;
        }

        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            m_memCache[m_userId] = QImage();
            emit finished();
            return;
        }

        // Save to disk cache (sanitized userId)
        QString safeId = m_userId;
        safeId.remove(QRegularExpression("[/\\\\\\.]"));
        if (safeId.isEmpty()) safeId = "_unknown";
        QString filePath = m_cachePath + "/" + safeId + ".png";
        img.save(filePath, "PNG");

        handleImage(img);
    });
}

void AvatarFetchResponse::handleImage(const QImage &image)
{
    int size = m_requestedSize.width() > 0 ? m_requestedSize.width() : 64;
    m_image = cropToCircle(image, size);

    // Store in memory cache for instant access next time
    if (!m_memCache.contains(m_userId))
        m_memCacheOrder.append(m_userId);
    m_memCache[m_userId] = m_image;

    // Evict oldest entries if over limit
    while (m_memCache.size() > AvatarProvider::MAX_MEM_CACHE && !m_memCacheOrder.isEmpty()) {
        QString evictId = m_memCacheOrder.takeFirst();
        m_memCache.remove(evictId);
    }

    emit finished();
}

QImage AvatarFetchResponse::cropToCircle(const QImage &src, int size)
{
    QImage scaled = src.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    int x = (scaled.width() - size) / 2;
    int y = (scaled.height() - size) / 2;
    if (x > 0 || y > 0)
        scaled = scaled.copy(x, y, size, size);

    QImage result(size, size, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    painter.setClipPath(path);
    painter.drawImage(0, 0, scaled);

    return result;
}

QQuickTextureFactory *AvatarFetchResponse::textureFactory() const
{
    return QQuickTextureFactory::textureFactoryForImage(m_image);
}
