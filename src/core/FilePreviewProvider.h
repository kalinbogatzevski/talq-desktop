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
class FilePreviewResponse : public QQuickImageResponse
{
    Q_OBJECT
public:
    FilePreviewResponse(int fileId, const QSize &requestedSize, ApiClient *api,
                        QHash<int, QImage> &cache);
    QQuickTextureFactory *textureFactory() const override;

private:
    QImage m_image;
};

class FilePreviewProvider : public QQuickAsyncImageProvider
{
public:
    explicit FilePreviewProvider(ApiClient *api);
    QQuickImageResponse *requestImageResponse(
        const QString &id, const QSize &requestedSize) override;

private:
    ApiClient *m_api;
    QHash<int, QImage> m_cache;
};
