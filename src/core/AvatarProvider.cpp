#include "core/AvatarProvider.h"
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
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

    return new AvatarFetchResponse(id, requestedSize, m_api, m_cachePath, m_memCache);
}

// --- AvatarCachedResponse ---

AvatarCachedResponse::AvatarCachedResponse(const QImage &image)
    : m_image(image)
{
    emit finished();
}

QQuickTextureFactory *AvatarCachedResponse::textureFactory() const
{
    return QQuickTextureFactory::textureFactoryForImage(m_image);
}

// --- AvatarFetchResponse ---

AvatarFetchResponse::AvatarFetchResponse(const QString &userId, const QSize &requestedSize,
                                         ApiClient *api, const QString &cachePath,
                                         QHash<QString, QImage> &memCache)
    : m_userId(userId)
    , m_requestedSize(requestedSize)
    , m_api(api)
    , m_cachePath(cachePath)
    , m_memCache(memCache)
{
    // Defer the work to next event loop iteration to avoid blocking the caller
    QMetaObject::invokeMethod(this, &AvatarFetchResponse::loadFromDisk, Qt::QueuedConnection);
}

void AvatarFetchResponse::loadFromDisk()
{
    QString filePath = m_cachePath + "/" + m_userId + ".png";
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

    auto *reply = m_api->getAbsoluteUrl("/index.php/avatar/" + m_userId + "/64");

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

        // Save to disk cache
        QString filePath = m_cachePath + "/" + m_userId + ".png";
        img.save(filePath, "PNG");

        handleImage(img);
    });
}

void AvatarFetchResponse::handleImage(const QImage &image)
{
    int size = m_requestedSize.width() > 0 ? m_requestedSize.width() : 64;
    m_image = cropToCircle(image, size);

    // Store in memory cache for instant access next time
    m_memCache[m_userId] = m_image;

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
