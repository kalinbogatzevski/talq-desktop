#pragma once

#include <QImage>
#include <QHash>

class ApiClient;

/**
 * Avatar cache manager. No longer a QML image provider.
 * Kept for debug monitoring (cache count).
 */
class AvatarProvider
{
public:
    explicit AvatarProvider(ApiClient *api) : m_api(api) {}

    int cacheCount() const { return m_memCache.size(); }

private:
    ApiClient *m_api;
    QHash<QString, QImage> m_memCache;
};
