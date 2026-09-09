#pragma once

// WebSocket transport to a CTI daemon.
//
// TalQ knows nothing about the phone system behind this. It receives
// technology-neutral events -- ring, end -- so a site running something other
// than Asterisk writes its own daemon and this class is unchanged. That is
// also why there is no TALQ_BRAND ifdef anywhere near it: the server URL is a
// setting, so the open-source build has the feature too.

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

class QWebSocket;

class CtiClient : public QObject
{
    Q_OBJECT

public:
    explicit CtiClient(QObject *parent = nullptr);
    ~CtiClient() override;

    // Why this exists: `m_ready == false` used to be the only thing a consumer
    // could see, and it covered six unrelated realities -- never started, first
    // connect in flight, retrying against a blocked port, latched off for a bad
    // credential, and so on. A site whose firewall blocked this port therefore
    // looked EXACTLY like a site that had never configured the feature, and the
    // UI could say nothing useful about either. Distinguishing them is the whole
    // point: "reconnecting" is reassuring and true for a blip, and a lie for a
    // policy block that will never come back on its own.
    enum class Health {
        Off,                  // stopped: disabled, unconfigured, or no token
        Connecting,           // trying, and has NEVER succeeded this run
        Reconnecting,         // was connected, dropped, retrying with backoff
        Connected,            // handshake done; events are flowing
        RefusedNoExtension,   // terminal: paired, but no extension is linked
        RefusedUnauthorised,  // terminal: the token is bad, revoked or expired
    };
    Q_ENUM(Health)

    Health health() const { return m_health; }

    // How long we have been in a non-Connected, non-Off state, in ms; 0 when
    // healthy or deliberately off. The UI uses this to stay quiet through a
    // blip and speak up once a fault has clearly outlived one.
    qint64 unhealthyForMs() const
    { return m_unhealthySince.isValid() ? m_unhealthySince.elapsed() : 0; }

    // Distinguishes "never worked on this machine" from "worked, then broke".
    // A first-ever connection that never lands points at configuration or a
    // network policy; a drop after success points at an outage.
    bool everConnected() const { return m_everConnected; }

    // Both are stored by the caller (QSettings); this class just uses them.
    void start(const QUrl &url, const QString &token);
    void stop();

    bool isConnected() const { return m_ready; }
    QString extension() const { return m_extension; }

    // Whether THIS site can place calls. Advertised by the daemon on `ready`,
    // because dialling needs a second, separately-permissioned credential the
    // operator has to configure. False everywhere it was not set up, and the
    // UI leaves the control out rather than offering one guaranteed to fail.
    bool canDial() const { return m_canDial; }

    // Ask the daemon to ring this desk phone and connect it to `number`.
    // The extension is NOT sent: the daemon takes it from the pairing, so a
    // client cannot place a call as somebody else.
    void dial(const QString &number);

signals:
    // The daemon accepted us and told us which extension we are watching.
    void ready(const QString &extension, const QString &displayName);
    void ringing(const QString &callId, const QString &caller, const QString &extension);
    void ended(const QString &callId, const QString &reason);

    // The socket dropped. Any card still on screen is now stale -- the call
    // may have been answered by a colleague minutes ago -- so the UI clears.
    void disconnected();

    // Terminal: the token is bad, revoked or expired. Retrying cannot fix it,
    // so the UI says so once instead of looping forever.
    void authenticationFailed(const QString &reason);

    // Every transition of health(), including the ones that produce no other
    // signal at all -- a first connect that never lands emitted NOTHING before
    // this existed, which is precisely why a blocked port was invisible.
    void healthChanged(CtiClient::Health health);

    // Outcome of a dial(). `detail` is a short machine reason -- ringing,
    // bad-number, too-soon, not-enabled, pbx-unreachable -- so the UI can say
    // something specific instead of "failed".
    void dialResult(bool ok, const QString &detail);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);

private:
    void scheduleReconnect();
    void setHealth(Health h);

    QWebSocket *m_socket = nullptr;
    QUrl m_url;
    QString m_token;
    QString m_extension;
    QTimer m_reconnectTimer;
    QTimer m_keepAliveTimer;
    QTimer m_idleTimer;

    bool m_running = false;        // start() called and not stopped
    bool m_canDial = false;
    bool m_ready = false;          // handshake completed
    bool m_authRejected = false;   // stop retrying; the credential is the problem
    int m_backoffMs = kMinBackoffMs;

    Health m_health = Health::Off;
    bool m_everConnected = false;
    QElapsedTimer m_unhealthySince;   // invalid while Connected or Off

    // A ringing phone is a several-second window, so a slow first reconnect
    // would silently miss calls; but a tight loop against a down daemon is
    // rude. Start fast, back off to a minute.
    static constexpr int kMinBackoffMs = 2000;
    static constexpr int kMaxBackoffMs = 60000;

    // Without a keepalive an idle NAT or firewall culls the connection while
    // Qt still reports ConnectedState, so the client sits there believing it
    // is subscribed and every later call rings the phone and pops nothing --
    // silently, for the rest of the session. Mirrors SignalingClient.
    static constexpr int kPingMs = 25000;
    // Two missed pongs plus slack. A screen-pop that is 60s late is useless,
    // so it is better to tear down and reconnect than to wait politely.
    static constexpr int kIdleMs = 70000;
};
