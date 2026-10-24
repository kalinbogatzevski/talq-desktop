#include "core/ReactionWhoService.h"

#include "core/ApiClient.h"
#include "core/TalqLog.h"

#include <QDateTime>

namespace {

// A hover asks for one message at a time, so this is only a ceiling against a
// pointer swept across many reaction bars faster than the server answers.
constexpr int kMaxInFlight = 4;
// Messages remembered. Each holds at most a few dozen short names.
constexpr int kMaxEntries  = 64;

} // namespace

namespace rw = talq::reactionwho;

ReactionWhoService::ReactionWhoService(QObject *parent)
    : QObject(parent)
{
}

void ReactionWhoService::setApi(ApiClient *api)
{
    m_api = api;
}

void ReactionWhoService::clear()
{
    ++m_generation;
    m_entries.clear();
    m_order.clear();
    m_declined = {};
    m_inFlight = 0;
}

QString ReactionWhoService::keyOf(const QString &token, int messageId)
{
    return token + QLatin1Char('/') + QString::number(messageId);
}

ReactionWhoService::Lookup
ReactionWhoService::lookup(const QString &token, int messageId, const QString &emoji,
                           const QString &signature) const
{
    Lookup out;
    const auto it = m_entries.constFind(keyOf(token, messageId));
    if (it == m_entries.constEnd())
        return out;
    // A held list is shown for as long as the pill looks the way it did when the
    // list was asked for, however old it is: request() refreshes it in the
    // background, and the tooltip is replaced if the answer differs.
    if (it->hasData && it->sig == signature) {
        out.state = State::Ready;
        out.actors = it->byEmoji.value(emoji);
        return out;
    }
    if (it->loading) {
        out.state = State::Loading;
        return out;
    }
    if (it->failed && !rw::mayRetryAfterFailure(it->failedAtMs, QDateTime::currentMSecsSinceEpoch()))
        out.state = State::Failed;
    return out;
}

void ReactionWhoService::request(const QString &token, int messageId, const QString &signature)
{
    if (!m_api)
        return;
    const QString path = rw::reactorsPath(token, messageId);
    if (path.isEmpty())
        return;                                       // not a real token or id

    const QString key = keyOf(token, messageId);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Entry &e = m_entries[key];
    if (!m_order.contains(key))
        m_order.append(key);

    if (e.loading)
        return;                                       // its answer is on the way
    if (e.hasData && rw::isFresh(e.sig, signature, e.atMs, now))
        return;
    if (e.failed && !rw::mayRetryAfterFailure(e.failedAtMs, now))
        return;
    if (m_inFlight >= kMaxInFlight) {
        m_declined = Declined{ token, messageId, signature };   // served when a slot frees
        return;
    }
    if (m_declined.messageId == messageId && m_declined.token == token)
        m_declined = {};                              // being served now

    e.loading = true;
    e.failed = false;
    ++m_inFlight;
    trim();

    const int gen = m_generation;
    QPointer<ReactionWhoService> self(this);
    m_api->get(path, [self, gen, key, token, messageId, signature](bool ok, const QJsonObject &data,
                                                                   int status) {
        if (!self || gen != self->m_generation)
            return;                                   // cleared (or gone) while in flight
        --self->m_inFlight;

        const auto it = self->m_entries.find(key);
        if (it != self->m_entries.end()) {
            it->loading = false;
            if (ok) {
                it->byEmoji = rw::parseReactors(data);
                it->sig = signature;                  // the pill as it was when we asked
                it->atMs = QDateTime::currentMSecsSinceEpoch();
                it->hasData = true;
                it->failed = false;
                if (TalqLog::g_verbose) {
                    int actors = 0;
                    for (auto a = it->byEmoji.constBegin(); a != it->byEmoji.constEnd(); ++a)
                        actors += int(a.value().size());
                    TLOG(QString("[reactionwho] msg %1: %2 emoji, %3 reactors")
                             .arg(messageId).arg(it->byEmoji.size()).arg(actors));
                }
            } else {
                it->failed = true;
                it->failedAtMs = QDateTime::currentMSecsSinceEpoch();
                if (TalqLog::g_verbose)
                    TLOG(QString("[reactionwho] msg %1: failed (status=%2)").arg(messageId).arg(status));
            }
        }
        emit self->reactorsReady(token, messageId);
        // A slot just freed: serve the request the cap turned away, if any.
        if (self)
            self->retryDeclined();
    });
}

void ReactionWhoService::retryDeclined()
{
    if (m_declined.messageId <= 0)
        return;
    const Declined d = m_declined;
    m_declined = {};
    // Either starts a fetch (its answer reaches the tooltip through reactorsReady),
    // finds the list already held or loading and returns, or is declined again and
    // remembered again. It cannot loop: it only runs when a fetch has just finished.
    request(d.token, d.messageId, d.signature);
}

void ReactionWhoService::trim()
{
    // The oldest settled entries go first; one still loading is never dropped from
    // under its own reply.
    int guard = int(m_order.size());
    while (m_entries.size() > kMaxEntries && guard-- > 0 && !m_order.isEmpty()) {
        const QString oldest = m_order.takeFirst();
        const auto it = m_entries.constFind(oldest);
        if (it == m_entries.constEnd())
            continue;
        if (it->loading) {
            m_order.append(oldest);
            continue;
        }
        m_entries.erase(it);
    }
}
