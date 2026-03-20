#pragma once

#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QImage>
#include <QHash>
#include "core/ApiClient.h"

/**
 * Immediately returns a cached image (already loaded).
 */
class AvatarCachedResponse : public QQuickImageResponse
{
public:
    AvatarCachedResponse(const QImage &image);
    QQuickTextureFactory *textureFactory() const override;
private:
    QImage m_image;
};

/**
 * Fetches an avatar from disk cache or server, crops to circle.
 */
class AvatarFetchResponse : public QQuickImageResponse
{
    Q_OBJECT
public:
    AvatarFetchResponse(const QString &userId, const QSize &requestedSize,
                        ApiClient *api, const QString &cachePath,
                        QHash<QString, QImage> &memCache);
    QQuickTextureFactory *textureFactory() const override;

private:
    void loadFromDisk();
    void fetchFromServer();
    void handleImage(const QImage &image);
    static QImage cropToCircle(const QImage &src, int size);

    QString m_userId;
    QSize m_requestedSize;
    ApiClient *m_api;
    QString m_cachePath;
    QImage m_image;
    QHash<QString, QImage> &m_memCache;
};

/**
 * QQuickAsyncImageProvider that serves user avatars via "image://avatar/{userId}".
 * Uses a 3-tier cache: memory → disk → server.
 */
class AvatarProvider : public QQuickAsyncImageProvider
{
public:
    explicit AvatarProvider(ApiClient *api);
    QQuickImageResponse *requestImageResponse(
        const QString &id, const QSize &requestedSize) override;

    int cacheCount() const { return m_memCache.size(); }

private:
    ApiClient *m_api;
    QString m_cachePath;
    QHash<QString, QImage> m_memCache;
};
