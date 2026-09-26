#pragma once

#include "core/ImageCopyLogic.h"

#include <QObject>
#include <QPointer>
#include <functional>

class ApiClient;
class MessageListModel;
class QByteArray;
class QImage;

// Puts an attachment's picture on the system clipboard. The ONE route both the
// chat's "Copy image" and the image viewer's Copy take, so the two can never
// hand over different pixels for the same file.
//
// The viewer used to copy the image it held, at that image's own size:
// /core/preview's re-render capped to the screen or, until that arrived, the
// chat's thumbnail of at most 800x600. This fetches the ORIGINAL instead, and
// falls back to the server's largest preview only when the original cannot be
// decoded here (see ImageCopyLogic.h for the order).
class ImageClipboard : public QObject
{
    Q_OBJECT
public:
    ImageClipboard(ApiClient *api, MessageListModel *messages, QObject *parent);

    // Fetch `fileId` and copy it. Asynchronous: `announce(ok)` runs once, when
    // this request finishes -- and not at all if it was superseded first, by a
    // newer copy() or by anything else putting something on the clipboard,
    // because the clipboard then belongs to that.
    void copy(int fileId, const QString &mime, std::function<void(bool ok)> announce);

    // Whether this process has a decoder for `mime`, following its aliases
    // (Nextcloud may report image/x-icon where the plugin registers
    // image/vnd.microsoft.icon).
    static bool canDecodeHere(const QString &mime);

private:
    void tryFrom(talq::ImageCopySource source, QObject *request, int fileId,
                 std::function<void(bool)> announce);
    void finish(QObject *request);
    static QImage decode(const QByteArray &bytes);

    ApiClient *m_api;
    MessageListModel *m_messages;
    // The running request's context: every fetch is bound to it, so deleting
    // it aborts the transfer and severs the callbacks.
    QPointer<QObject> m_inflight;
    quint64 m_clipboardAtStart = 0;
};
