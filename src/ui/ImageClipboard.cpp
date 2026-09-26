#include "ImageClipboard.h"

#include "core/ApiClient.h"
#include "models/MessageListModel.h"

#include <QBuffer>
#include <QClipboard>
#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMimeDatabase>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
// The largest edge asked of /core/preview when the original cannot be used. The
// server clamps it to its own preview_max_x/y, so this only means "as large as
// you will render it".
constexpr int kPreviewFallbackDim = 4096;
// Inactivity timeout on the preview fallback. Longer than the original's 30 s
// (see fetchFileBytes): the server may render a large preview before it sends
// the first byte.
constexpr int kPreviewTimeoutMs = 60'000;

// Bumped by the system on every clipboard write, by any process. A copy that
// sees it move while its fetch was running stands down rather than overwrite
// what the user copied since. Windows only; elsewhere this is 0 and the check
// never fires, so a slow copy can still win there.
quint64 clipboardSequence()
{
#ifdef Q_OS_WIN
    return GetClipboardSequenceNumber();
#else
    return 0;
#endif
}
} // namespace

ImageClipboard::ImageClipboard(ApiClient *api, MessageListModel *messages, QObject *parent)
    : QObject(parent), m_api(api), m_messages(messages)
{
}

bool ImageClipboard::canDecodeHere(const QString &mime)
{
    // Asked of the decoders actually loaded into THIS process -- the image
    // plugins differ between a dev tree and an installed build -- rather than
    // of a list of types someone believed were supported.
    const QList<QByteArray> supported = QImageReader::supportedMimeTypes();
    if (supported.contains(mime.toLatin1()))
        return true;
    const QMimeType type = QMimeDatabase().mimeTypeForName(mime);   // resolves aliases
    if (!type.isValid())
        return false;
    if (supported.contains(type.name().toLatin1()))
        return true;
    const QStringList aliases = type.aliases();
    for (const QString &alias : aliases)
        if (supported.contains(alias.toLatin1()))
            return true;
    return false;
}

void ImageClipboard::copy(int fileId, const QString &mime, std::function<void(bool)> announce)
{
    // Supersede whatever is still running: its transfer is aborted rather than
    // left to finish and be thrown away (a held Ctrl+C once started one full
    // download per key repeat).
    delete m_inflight;
    auto *request = new QObject(this);
    m_inflight = request;
    m_clipboardAtStart = clipboardSequence();
    tryFrom(talq::firstImageCopySource(canDecodeHere(mime)), request, fileId,
            std::move(announce));
}

void ImageClipboard::finish(QObject *request)
{
    if (m_inflight == request)
        m_inflight = nullptr;
    request->deleteLater();   // we are inside a callback bound to it
}

void ImageClipboard::tryFrom(talq::ImageCopySource source, QObject *request, int fileId,
                             std::function<void(bool)> announce)
{
    using talq::ImageCopySource;
    if (request != m_inflight)
        return;   // superseded while the last step ran

    const auto fallBack = [this, source, request, fileId, announce](const QString &why) {
        qWarning() << "ImageClipboard: file" << fileId
                   << (source == ImageCopySource::Original ? "original" : "preview")
                   << "unusable:" << why;
        tryFrom(talq::nextImageCopySource(source), request, fileId, announce);
    };
    const auto deliver = [this, request, fileId, announce](const QImage &image, const char *from) {
        finish(request);
        if (clipboardSequence() != m_clipboardAtStart) {
            qInfo() << "ImageClipboard: file" << fileId
                    << "not copied: the clipboard changed while it was being fetched";
            return;
        }
        QGuiApplication::clipboard()->setImage(image);
        qDebug() << "ImageClipboard: copied file" << fileId << "from" << from << image.size();
        announce(true);
    };

    switch (source) {
    case ImageCopySource::Original:
        m_messages->fetchFileBytes(fileId, talq::kImageCopyMaxOriginalBytes, request,
            [this, request, fallBack, deliver](const QByteArray &bytes, const QString &error) {
            if (request != m_inflight) return;
            if (!error.isEmpty()) { fallBack(error); return; }
            const QImage image = decode(bytes);
            if (image.isNull()) { fallBack(QStringLiteral("did not decode")); return; }
            deliver(image, "original");
        });
        return;
    case ImageCopySource::Preview:
        m_api->fetchFileImage(fileId, kPreviewFallbackDim, request,
            [this, request, fallBack, deliver](const QImage &image, const QString &error) {
            if (request != m_inflight) return;
            if (image.isNull()) { fallBack(error); return; }
            deliver(image, "preview");
        }, kPreviewTimeoutMs);
        return;
    case ImageCopySource::Failed:
        finish(request);
        announce(false);
        return;
    }
}

QImage ImageClipboard::decode(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    // A phone photo is stored as the sensor saw it, with an EXIF note saying
    // which way is up. The server's preview -- what the chat and the viewer
    // show -- applies that note, so the original must too, or it would paste
    // turned on its side relative to the picture the user just right-clicked.
    reader.setAutoTransform(true);
    return reader.read();
}
