#pragma once

#include <QImage>
#include <QHash>

class ApiClient;

/**
 * File preview cache manager. No longer a QML image provider.
 * Kept for debug monitoring (cache count/bytes).
 */
class FilePreviewProvider
{
public:
    explicit FilePreviewProvider(ApiClient *api) : m_api(api) {}

    int cacheCount() const { return m_cache.size(); }
    qint64 cacheBytes() const { return m_cachedBytes; }

private:
    ApiClient *m_api;
    QHash<int, QImage> m_cache;
    qint64 m_cachedBytes = 0;
};
