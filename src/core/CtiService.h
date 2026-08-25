#pragma once

// Ties the CTI transport, the customer lookup and the on-screen cards
// together, so MainWindow only has to construct one object.
//
// Nothing here is brand-specific. The server URL is a setting, so the generic
// build has the feature and any organisation can point it at their own
// implementation of the protocol.

#include "core/CtiEventLogic.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include "painter/PainterTheme.h"

class CallerCardPopup;
class CtiClient;
class QNetworkAccessManager;

class CtiService : public QObject
{
    Q_OBJECT

public:
    explicit CtiService(QObject *parent = nullptr);
    ~CtiService() override;

    // Reads settings and connects, or stays dormant when unconfigured. Safe to
    // call again after settings change.
    void start();
    void stop();

    bool isEnabled() const;
    bool isConnected() const;
    QString extension() const;

    void setTheme(PainterTheme::Theme theme);

    // ── Settings ────────────────────────────────────────────────────────
    static bool enabledSetting();
    static void setEnabledSetting(bool on);
    static QUrl serverUrl();                       // wss://host:port
    static void setServerUrl(const QUrl &url);
    static QUrl erpBaseUrl();                      // https://erp.example
    static void setErpBaseUrl(const QUrl &url);
    static QString token();
    static void setToken(const QString &token);
    static void clearToken();

    // ── Pairing (Login-Flow-v2 style) ───────────────────────────────────
    // Asks the ERP to start a pairing, opens the approval page in the
    // browser, then polls until the user approves. No password ever enters
    // TalQ: approval happens in the browser session they are already
    // signed into.
    void beginPairing();
    void cancelPairing();

signals:
    void statusChanged();
    void pairingMessage(const QString &message, bool isError);
    void pairingSucceeded(const QString &displayName, const QString &extension);

private slots:
    void onRinging(const QString &callId, const QString &caller, const QString &extension);
    void onEnded(const QString &callId, const QString &reason);
    void onDisconnectedFromDaemon();
    void onAuthenticationFailed(const QString &reason);

private:
    void lookupCustomer(const QString &callId, const QString &caller);
    void dropCard(const QString &callId, int lingerMs);
    void repositionCards();
    void pollPairing();

    CtiClient *m_client = nullptr;
    QNetworkAccessManager *m_net = nullptr;

    talq::CtiCardStore m_store;

    // Insertion-ordered, NOT a QHash. Two reasons: QHash iteration order is
    // unspecified and per-process randomised, so the on-screen stacking order
    // of two simultaneous calls would shuffle; and a card that is lingering
    // after a missed call must stay in this list until its timer fires, or
    // every handler that resolves a card through it (dismiss, open, theme,
    // reposition, the stale-card wipe) silently does nothing while the card
    // is still on screen.
    struct CardEntry {
        QString callId;
        QPointer<CallerCardPopup> card;
        bool lingering = false;   // call over; visible but no longer live
    };
    QList<CardEntry> m_cards;

    CallerCardPopup *cardFor(const QString &callId) const;
    void removeCard(const QString &callId);

    QString m_pairToken;
    int m_pairPollsLeft = 0;
    int m_pairPollMs = 2000;   // overridden by the server's poll_interval

    PainterTheme::Theme m_theme = PainterTheme::Theme::Vivid;
};
