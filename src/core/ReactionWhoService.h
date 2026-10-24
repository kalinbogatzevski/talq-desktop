#pragma once

// Who reacted -- the network half. The reasoning (parsing, what the tooltip may
// say, when a list is stale) lives in ReactionWhoLogic.h, pure and tested.
//
// ChatPainter asks for a message's reactors when the pointer rests on one of its
// reaction pills, which is a hover and a tooltip, never a paint: request() may
// start an HTTP call, lookup() never does. One call serves every pill of the
// message (the answer is keyed by emoji), and a list is only shown while the pill
// still looks the way it did when the list was fetched.
//
// Guarded like the other services: a reply is dropped if clear() ran meanwhile
// (generation counter), and the service holds no pointer it would call after the
// ApiClient is gone (QPointer).

#include "core/ReactionWhoLogic.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

class ApiClient;

class ReactionWhoService : public QObject
{
    Q_OBJECT

public:
    explicit ReactionWhoService(QObject *parent = nullptr);

    // Not owned.
    void setApi(ApiClient *api);

    // Forget everything and drop replies still in flight: a list fetched with one
    // account's credentials is not shown to the next.
    void clear();

    enum class State {
        None,       // nothing known, nothing in flight (or a failure old enough to retry)
        Loading,    // a fetch is in flight and nothing matching the pill is held
        Ready,      // a list fetched under the pill's current signature is held
        Failed,     // the last fetch failed a moment ago
    };
    struct Lookup {
        State state = State::None;
        QVector<talq::reactionwho::Actor> actors;   // Ready only: this emoji's reactors
    };

    // Paint-safe: never starts a request. `signature` is what the pill looks like
    // now (talq::reactionwho::signatureOf); a list fetched under another one is not
    // returned, because the people behind a different count are a different list.
    Lookup lookup(const QString &token, int messageId, const QString &emoji,
                  const QString &signature) const;

    // "The pointer is on a pill of this message." Starts a fetch unless one is in
    // flight, a fresh list is held, or a failure is too recent. Idempotent and cheap
    // to call on every mouse move. Not for a paint path.
    void request(const QString &token, int messageId, const QString &signature);

signals:
    // A fetch for this message finished -- with a list or with a failure. The
    // tooltip that was showing "Loading…" is replaced by lookup()'s answer.
    void reactorsReady(const QString &token, int messageId);

private:
    struct Entry {
        bool    hasData = false;
        QString sig;                // the signature the held list was requested under
        qint64  atMs = 0;           // when it arrived
        bool    loading = false;
        bool    failed = false;
        qint64  failedAtMs = 0;
        talq::reactionwho::ByEmoji byEmoji;
    };

    static QString keyOf(const QString &token, int messageId);
    void trim();

    // The most recent request the in-flight cap turned away. It is the pill the
    // pointer is most likely on NOW (hover asks again on every move, so the last
    // one declined is the newest), and without this a tooltip that opened on
    // "Loading…" while every slot was busy had nothing to wake it: the slot that
    // freed belonged to some other message.
    struct Declined {
        QString token;
        int     messageId = 0;
        QString signature;
    };
    void retryDeclined();

    QPointer<ApiClient>   m_api;
    Declined              m_declined;
    QHash<QString, Entry> m_entries;
    QList<QString>        m_order;          // insertion order, for trimming
    int                   m_inFlight = 0;
    int                   m_generation = 0; // bumped by clear(): late replies are dropped
};
