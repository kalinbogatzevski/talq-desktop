#pragma once

// Colleague shift status -- the network half.
//
// All of the reasoning lives in ShiftStatusLogic.h, which is pure and tested.
// This owns three things and nothing else: a cache keyed on the colleague's
// Nextcloud user id, one timer, and one HTTP call.
//
// The division of labour with the painters matters. Painters DECLARE what is
// on screen via observe(); this service decides what to fetch and when. A
// painter must never start a network request from inside a paint path -- paint
// runs on every scroll, resize and repaint, and a fetch there would be
// unbounded.
//
// Nothing here is brand-specific. The ERP address and the credential are
// settings shared with CtiService, so the generic build has the feature and
// any organisation can point it at their own implementation of the protocol.

#include "core/ShiftStatusLogic.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

class QNetworkAccessManager;

class ShiftStatusService : public QObject
{
    Q_OBJECT

public:
    explicit ShiftStatusService(QObject *parent = nullptr);

    // Never blocks and never fetches -- safe to call from a paint path.
    // An absent colleague is Unknown, which draws nothing.
    talq::ShiftState stateFor(const QString &userId) const;
    QString          labelFor(const QString &userId) const;

    // "These colleagues are on screen." Coalesced into the next batch; a
    // colleague who scrolls out simply stops being refreshed.
    void observe(const QStringList &userIds);

    // Idempotent, and safe to call again after settings change.
    void start();
    void stop();

signals:
    // Emitted once per applied batch, and only when something actually
    // changed -- a repaint of every surface is not free.
    void statusesChanged();

private:
    struct Entry {
        talq::ShiftState state = talq::ShiftState::Unknown;
        QString          label;
        qint64           fetchedAtMs = 0;
    };

    void fetchDue();
    void applyReply(const QByteArray &body, const QStringList &asked);

    QHash<QString, Entry> m_cache;      // Nextcloud user id -> entry
    QSet<QString>         m_observed;   // what the painters say is on screen
    QTimer                m_timer;
    QNetworkAccessManager *m_net = nullptr;
    bool                  m_inFlight = false;

    // Latched on 401/403, cleared only by start().
    //
    // A revoked key must not re-issue a failing request every two minutes for
    // the rest of the session. The token alone cannot tell us the key is dead:
    // CtiService::onAuthenticationFailed only toasts and never calls
    // clearToken(), so a non-empty token does NOT mean "paired".
    bool                  m_authRejected = false;
};
