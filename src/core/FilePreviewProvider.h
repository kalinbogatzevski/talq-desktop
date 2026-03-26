#pragma once

#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QImage>
#include <QHash>
#include "core/ApiClient.h"

/**
 * Fetches a file preview from the NC server with authentication.
 * Usage in QML: source: "image://preview/" + fileId
 */
class FilePreviewProvider;

class FilePreviewResponse : public QQuickImageResponse
{
    Q_OBJECT
public:
    FilePreviewResponse(int fileId, const QSize &requestedSize, ApiClient *api,
                        QHash<int, QImage> &cache, FilePreviewProvider *provider);
    QQuickTextureFactory *textureFactory() const override;

private:
    Q_INVOKABLE void doFetch();

    QImage m_image;
    int m_fileId = 0;
    QSize m_requestedSize;
    ApiClient *m_api = nullptr;
    QHash<int, QImage> &m_cache;
    FilePreviewProvider *m_provider = nullptr;
};

class FilePreviewProvider : public QQuickAsyncImageProvider
{
public:
    explicit FilePreviewProvider(ApiClient *api);
    QQuickImageResponse *requestImageResponse(
        const QString &id, const QSize &requestedSize) override;

    int cacheCount() const { return m_cache.size(); }
    qint64 cacheBytes() const;
    void evictIfNeeded();

    static constexpr qint64 MAX_CACHE_BYTES = 50 * 1024 * 1024;  // 50MB

private:
    friend class FilePreviewResponse;
    ApiClient *m_api;
    QHash<int, QImage> m_cache;
    QList<int> m_cacheOrder;
    qint64 m_cachedBytes = 0;
};
