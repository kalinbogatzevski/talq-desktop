#include "core/LinkPreviewService.h"

#include "core/ApiClient.h"
#include "core/TalqLog.h"

#include <QBuffer>
#include <QDateTime>
#include <QImageReader>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int    kMaxResolveInFlight = 3;
constexpr int    kMaxThumbInFlight   = 2;
constexpr int    kMaxEntries         = 300;
constexpr int    kMaxQueued          = 32;
// A page's og:image is ~1200x630; the card shows 96x64 logical pixels. The caps are
// generous for that and small enough that decoding one on the GUI thread costs
// tens of milliseconds and a bounded amount of memory, whatever the page serves.
constexpr qint64 kThumbMaxBytes      = 2 * 1024 * 1024;
constexpr qint64 kThumbMaxPixels     = 8 * 1000 * 1000;
constexpr int    kThumbW             = 192;            // 96 x 64 logical px at 2x
constexpr int    kThumbH             = 128;
constexpr int    kThumbTimeoutMs     = 20000;
constexpr int    kThumbRetryMs       = 8000;

// "The page has no preview" is final for a good while; "the server did not
// answer" is worth another try soon.
constexpr qint64 kRetryDefinitiveMs  = 30 * 60 * 1000;
constexpr qint64 kRetryTransientMs   = 2 * 60 * 1000;

} // namespace

LinkPreviewService::LinkPreviewService(QObject *parent)
    : QObject(parent)
{
}

void LinkPreviewService::setApi(ApiClient *api)
{
    m_api = api;
}

void LinkPreviewService::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    if (!on)
        clear();
}

void LinkPreviewService::clear()
{
    ++m_generation;
    m_entries.clear();
    m_order.clear();
    m_queue.clear();
    m_thumbQueue.clear();
    m_resolveInFlight = 0;
    m_thumbInFlight = 0;
}

QVector<talq::linkpreview::ReadyCard>
LinkPreviewService::readyCards(const QStringList &urls) const
{
    QVector<talq::linkpreview::ReadyCard> out;
    if (!m_enabled)
        return out;
    for (const QString &u : urls) {
        const auto it = m_entries.constFind(u);
        if (it == m_entries.constEnd() || it->state != Entry::Ready)
            continue;
        talq::linkpreview::ReadyCard rc;
        rc.data = it->data;
        rc.hasThumb = !it->thumbPath.isEmpty();
        out.append(rc);
    }
    return out;
}

QImage LinkPreviewService::thumb(const QString &url) const
{
    if (!m_enabled)
        return {};
    const auto it = m_entries.constFind(url);
    return it == m_entries.constEnd() ? QImage() : it->thumb;
}

QString LinkPreviewService::signatureFor(const QStringList &urls) const
{
    if (!m_enabled || urls.isEmpty())
        return {};
    QString sig;
    for (const QString &u : urls) {
        const auto it = m_entries.constFind(u);
        if (it == m_entries.constEnd() || it->state != Entry::Ready) {
            sig += QLatin1Char('-');
        } else {
            sig += QLatin1Char('R');
            sig += QString::number(
                qulonglong(qHash(it->data.title)) ^ (qulonglong(qHash(it->data.description)) << 1), 16);
            sig += it->thumbPath.isEmpty() ? QLatin1Char('n') : QLatin1Char('t');
        }
        sig += QLatin1Char(',');
    }
    return sig;
}

void LinkPreviewService::observe(const QStringList &urls)
{
    if (!m_enabled || !m_api || urls.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool queued = false;
    for (const QString &u : urls) {
        auto it = m_entries.find(u);
        if (it != m_entries.end()) {
            // A "no preview" answer is asked again once it is old enough.
            if (it->state == Entry::None && now >= it->retryAtMs) {
                m_entries.erase(it);
                m_order.removeOne(u);
            } else {
                continue;
            }
        }
        // Already waiting: it is on screen again, so it moves to the front of the
        // line (the pump serves the newest request first).
        const qsizetype qi = m_queue.indexOf(u);
        if (qi >= 0) {
            m_queue.move(qi, m_queue.size() - 1);
            continue;
        }
        m_queue.append(u);
        queued = true;
    }
    // Only what was on screen recently is worth keeping: a long scroll must not
    // leave a queue of rows that are long gone, resolved before the ones in view.
    while (m_queue.size() > kMaxQueued)
        m_queue.removeFirst();
    if (queued)
        schedulePump();
}

void LinkPreviewService::schedulePump()
{
    if (m_pumpScheduled)
        return;
    m_pumpScheduled = true;
    // From the event loop, never from the caller's stack: observe() is called
    // while painting.
    QTimer::singleShot(0, this, [this]() {
        m_pumpScheduled = false;
        pump();
    });
}

void LinkPreviewService::pump()
{
    if (!m_enabled || !m_api)
        return;
    // Newest request first: what the user is looking at now is what was
    // observed last, and a long queue built while scrolling should serve the
    // rows still on screen before the ones already scrolled past.
    while (m_resolveInFlight < kMaxResolveInFlight && !m_queue.isEmpty()) {
        const QString url = m_queue.takeLast();
        if (m_entries.contains(url))
            continue;
        startResolve(url);
    }
}

void LinkPreviewService::startResolve(const QString &url)
{
    Entry e;
    e.state = Entry::Loading;
    m_entries.insert(url, e);
    m_order.append(url);
    ++m_resolveInFlight;
    trim();

    const int gen = m_generation;
    QPointer<LinkPreviewService> self(this);
    // The address goes into the path already percent-encoded: ApiClient::get
    // parses the path as a URL, and QUrlQuery would leave a '+' for the server
    // to read as a space.
    const QString path = QStringLiteral("/references/resolve?reference=")
        + QString::fromLatin1(QUrl::toPercentEncoding(url));

    m_api->get(path, [self, gen, url](bool ok, const QJsonObject &data, int status) {
        if (!self || gen != self->m_generation)
            return;                                   // cleared (or gone) while in flight
        --self->m_resolveInFlight;

        auto it = self->m_entries.find(url);
        if (it == self->m_entries.end()) {
            self->pump();
            return;
        }

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        talq::linkpreview::CardData card;
        if (ok)
            card = talq::linkpreview::parseResolveResponse(data, url);

        if (card.valid()) {
            it->state = Entry::Ready;
            it->data = card;
            it->thumbPath = self->m_api
                ? talq::linkpreview::thumbPathFor(card.thumbUrl, self->m_api->serverUrl())
                : QString();
            const QString thumbPath = it->thumbPath;
            emit self->cardReady(url);
            if (!thumbPath.isEmpty())
                self->startThumb(url, thumbPath);
        } else {
            // Nothing to show. No signal: nothing was reserved for this row, so
            // there is nothing to re-lay-out.
            it->state = Entry::None;
            const bool transient = !ok && (status == 0 || status == 429 || status >= 500);
            it->retryAtMs = now + (transient ? kRetryTransientMs : kRetryDefinitiveMs);
            if (TalqLog::g_verbose)
                TLOG(QString("[linkpreview] no card for %1 (ok=%2 status=%3)")
                         .arg(url.left(80)).arg(ok).arg(status));
        }
        self->pump();
    });
}

void LinkPreviewService::startThumb(const QString &url, const QString &path)
{
    // Pictures wait their turn like everything else; a card whose picture never
    // arrives (or is refused) simply keeps its blank picture box.
    m_thumbQueue.append({url, path});
    pumpThumbs();
}

void LinkPreviewService::pumpThumbs()
{
    while (m_api && m_thumbInFlight < kMaxThumbInFlight && !m_thumbQueue.isEmpty()) {
        const auto job = m_thumbQueue.takeFirst();
        if (!m_entries.contains(job.first))
            continue;
        fetchThumb(job.first, job.second);
    }
}

void LinkPreviewService::fetchThumb(const QString &url, const QString &path)
{
    ++m_thumbInFlight;
    const int gen = m_generation;

    QNetworkReply *reply = m_api->getAbsoluteUrl(path, kThumbTimeoutMs);
    ApiClient::bindReplyLifetime(reply, this);

    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64) {
        if (received > kThumbMaxBytes)
            reply->abort();                           // an enormous "thumbnail" is not worth the memory
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, gen]() {
        if (gen != m_generation)
            return;                                   // cleared: the counters were reset with it
        --m_thumbInFlight;
        const auto finish = qScopeGuard([this]() { pumpThumbs(); });

        auto it = m_entries.find(url);
        if (it == m_entries.end())
            return;
        if (reply->error() != QNetworkReply::NoError) {
            // A timeout, a dropped connection, 429 or 5xx is worth one more try
            // (a card should not keep a blank picture box for the whole session
            // because one request was unlucky); a refusal or an over-size abort is not.
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool transient = reply->error() != QNetworkReply::OperationCanceledError
                && (status == 0 || status == 429 || status >= 500);
            if (transient && it->thumbTries < 2) {
                ++it->thumbTries;
                const QString path = it->thumbPath;
                QTimer::singleShot(kThumbRetryMs, this, [this, url, path, gen]() {
                    if (gen == m_generation && m_entries.contains(url))
                        startThumb(url, path);
                });
            }
            return;
        }

        QByteArray bytes = reply->readAll();
        if (bytes.isEmpty() || bytes.size() > kThumbMaxBytes)
            return;
        QBuffer buf(&bytes);
        buf.open(QIODevice::ReadOnly);
        QImageReader reader(&buf);
        reader.setAutoTransform(true);
        const QSize sz = reader.size();
        if (!sz.isValid() || qint64(sz.width()) * sz.height() > kThumbMaxPixels)
            return;
        const QImage img = reader.read();
        if (img.isNull())
            return;

        // A centre-cropped "cover" fill: the card's picture box has one fixed
        // shape whatever the page's image was.
        const QImage scaled = img.scaled(kThumbW, kThumbH, Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation);
        const QRect crop((scaled.width() - kThumbW) / 2, (scaled.height() - kThumbH) / 2,
                         kThumbW, kThumbH);
        it->thumb = scaled.copy(crop).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        emit thumbReady(url);
    });
}

void LinkPreviewService::trim()
{
    // Bound the cache; the oldest settled entries go first. An entry still
    // loading is never dropped from under its own reply.
    int guard = m_order.size();
    while (m_entries.size() > kMaxEntries && guard-- > 0 && !m_order.isEmpty()) {
        const QString oldest = m_order.takeFirst();
        const auto it = m_entries.constFind(oldest);
        if (it == m_entries.constEnd())
            continue;
        if (it->state == Entry::Loading) {
            m_order.append(oldest);
            continue;
        }
        m_entries.erase(it);
    }
}
