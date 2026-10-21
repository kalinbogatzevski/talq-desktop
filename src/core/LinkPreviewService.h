#pragma once

// Link previews -- the network half.
//
// All the reasoning lives in LinkPreviewLogic.h (pure, tested). This owns a
// cache keyed on the URL, a small request queue, and two kinds of HTTP call to
// the user's own Nextcloud server: references/resolve (title, description,
// thumbnail address) and the thumbnail itself.
//
// The division of labour with ChatPainter is the one ShiftStatusService has:
// the painter DECLARES which addresses are on screen via observe(), and this
// service decides what to fetch and when. observe() only queues -- the request
// itself starts from the event loop -- so a paint path never opens a socket,
// however often it paints.
//
// Everything a lookup returns is safe to call from a paint path; nothing blocks.

#include "core/LinkPreviewLogic.h"
#include "painter/LinkCardLayout.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

class ApiClient;

class LinkPreviewService : public QObject
{
    Q_OBJECT

public:
    explicit LinkPreviewService(QObject *parent = nullptr);

    // Not owned. The service never outlives the app's ApiClient in practice, but
    // it guards its replies with a QPointer to itself and a generation counter,
    // never with the client.
    void setApi(ApiClient *api);

    // Off: observe() does nothing and every lookup answers "no card", so the
    // chat looks exactly as it did before the feature existed.
    void setEnabled(bool on);
    bool enabled() const { return m_enabled; }

    // ── Paint-safe lookups ──
    // The cards the server has answered for, among `urls` (in order).
    QVector<talq::linkpreview::ReadyCard> readyCards(const QStringList &urls) const;
    // The card's picture, or a null image while it is loading, failed or absent.
    QImage thumb(const QString &url) const;
    // Changes whenever anything that affects the LAYOUT of a row with these
    // addresses changes -- the layout cache is keyed on it.
    QString signatureFor(const QStringList &urls) const;

    // "These addresses are on screen." Cheap and idempotent.
    void observe(const QStringList &urls);

    // Forget everything (a different account, or the feature switched off then
    // on): a preview fetched with one user's credentials is not shown to another.
    void clear();

signals:
    // A card appeared or changed shape -- rows that show this address must be
    // laid out again.
    void cardReady(const QString &url);
    // Only the picture arrived -- a repaint is enough.
    void thumbReady(const QString &url);

private:
    struct Entry {
        enum State { Loading, Ready, None };
        State  state = Loading;
        talq::linkpreview::CardData data;
        QString thumbPath;          // accepted by thumbPathFor(); empty = no picture
        QImage  thumb;
        int     thumbTries = 0;     // transient picture failures retried so far
        qint64  retryAtMs = 0;      // for None: when it may be asked again
    };

    void schedulePump();
    void pump();
    void startResolve(const QString &url);
    void startThumb(const QString &url, const QString &path);
    void pumpThumbs();
    void fetchThumb(const QString &url, const QString &path);
    void trim();

    QPointer<ApiClient>   m_api;
    QHash<QString, Entry> m_entries;
    QList<QString>        m_order;       // insertion order, for trimming
    QList<QString>        m_queue;       // addresses waiting for a free slot
    QList<QPair<QString, QString>> m_thumbQueue;   // (address, thumbnail path)
    int                   m_resolveInFlight = 0;
    int                   m_thumbInFlight = 0;
    int                   m_generation = 0;     // bumped by clear(): late replies are dropped
    bool                  m_pumpScheduled = false;
    bool                  m_enabled = true;
};
