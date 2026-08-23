#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QTimer>
#include "core/ApiClient.h"
#include "core/ServerCapabilities.h"

/**
 * Manages Nextcloud Login Flow v2 authentication — no embedded browser needed.
 *
 * Flow:
 *  1. User enters server URL
 *  2. We verify it's Nextcloud with Talk via capabilities
 *  3. POST {server}/index.php/login/v2 → returns { login: URL, poll: { token, endpoint } }
 *  4. Open the login URL in the user's default system browser
 *  5. Poll the endpoint every 2s until user completes login
 *  6. Poll response returns { server, loginName, appPassword }
 *  7. Credentials are persisted to QSettings
 */
class AuthManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loggedIn READ isLoggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY userInfoChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY userInfoChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)
    Q_PROPERTY(bool waitingForBrowser READ isWaitingForBrowser NOTIFY waitingChanged)
    Q_PROPERTY(bool restoringSession READ isRestoringSession NOTIFY restoringChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    Q_PROPERTY(QString nextcloudVersion READ nextcloudVersion NOTIFY serverInfoChanged)
    Q_PROPERTY(QString talkVersion READ talkVersion NOTIFY serverInfoChanged)
    Q_PROPERTY(QString signalingMode READ signalingMode NOTIFY serverInfoChanged)
    Q_PROPERTY(bool hasThreadsSupport READ hasThreadsSupport NOTIFY serverInfoChanged)

public:
    explicit AuthManager(ApiClient *api, QObject *parent = nullptr);

    bool isLoggedIn() const { return m_loggedIn; }
    QString serverUrl() const { return m_serverUrl; }
    QString displayName() const { return m_displayName; }
    QString userId() const { return m_userId; }
    QString errorMessage() const { return m_error; }
    bool isWaitingForBrowser() const { return m_waitingForBrowser; }
    bool isRestoringSession() const { return m_restoringSession; }
    QString statusMessage() const { return m_status; }
    QString nextcloudVersion() const { return m_ncVersion; }
    QString talkVersion() const { return m_talkVersion; }
    QString signalingMode() const { return m_signalingMode; }
    bool hasThreadsSupport() const { return hasCapability(QStringLiteral("threads")); }

    // --- server feature gating -------------------------------------------
    // The whole `spreed.features` list, parsed once per session. Before 0.65.0
    // this array was read for the single literal "threads" and then discarded,
    // so nothing else could be gated; every Talk 24 feature added in 0.65.0
    // goes through here and falls back to the 0.64 behaviour when absent.
    // Fails CLOSED — see ServerCapabilities.h.
    bool hasCapability(const QString &feature) const
    {
        return m_capabilities.has(feature.toStdString());
    }
    const talq::ServerCapabilities &capabilities() const { return m_capabilities; }

    // --- server-side config (capabilities `spreed.config`) ----------------
    // The user's and admin's server preferences, e.g.
    //   config.attachments.folder            where uploads belong
    //   config.chat.read-privacy             0 = public, 1 = private
    //   config.chat.typing-privacy           same encoding
    //   config.call.recording                whether recording is available
    //   config.conversations.can-create      whether this user may create rooms
    //
    // Every accessor takes a fallback and returns it when the section or key is
    // absent, because an older Talk sends only a fraction of these. Pick the
    // fallback to mean "behave the way TalQ did before this was readable", so a
    // missing key can never change existing behaviour.
    bool configBool(const QString &section, const QString &key, bool fallback) const;
    int configInt(const QString &section, const QString &key, int fallback) const;
    QString configString(const QString &section, const QString &key,
                         const QString &fallback = QString()) const;

    // Talk encodes both privacy settings as 0 = public / 1 = private
    // (Participant::PRIVACY_PUBLIC). Absent -> public, which is what TalQ did
    // unconditionally before it could read them.
    bool sharesReadStatus() const { return configInt(QStringLiteral("chat"),
                                                     QStringLiteral("read-privacy"), 0) == 0; }
    bool sharesTypingStatus() const { return configInt(QStringLiteral("chat"),
                                                       QStringLiteral("typing-privacy"), 0) == 0; }
    // Empty means "server did not say" -> callers keep the historic "Talk/".
    QString attachmentFolder() const { return configString(QStringLiteral("attachments"),
                                                           QStringLiteral("folder")); }
    bool canCreateConversations() const { return configBool(QStringLiteral("conversations"),
                                                            QStringLiteral("can-create"), true); }
    bool callsEnabledOnServer() const { return configBool(QStringLiteral("call"),
                                                          QStringLiteral("enabled"), true); }
    bool recordingAvailable() const { return configBool(QStringLiteral("call"),
                                                        QStringLiteral("recording"), false); }
    bool recordingConsentRequired() const { return configBool(QStringLiteral("call"),
                                                              QStringLiteral("recording-consent"), false); }
    bool supportsConversationTags() const { return m_capabilities.supportsConversationTags(); }
    bool supportsConversationPresets() const { return m_capabilities.supportsConversationPresets(); }
    bool supportsVoiceRooms() const { return m_capabilities.supportsVoiceRooms(); }

    Q_INVOKABLE void tryRestore();
    Q_INVOKABLE void startLogin(const QString &serverUrl);
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void logout();

signals:
    void loggedInChanged();
    void serverUrlChanged();
    void userInfoChanged();
    void errorChanged();
    void waitingChanged();
    void restoringChanged();
    void statusChanged();
    void serverInfoChanged();

private:
    void initiateLoginFlow();
    void pollForCredentials();
    void fetchUserInfo();
    void fetchServerInfo();
    void setError(const QString &msg);
    void setStatus(const QString &msg);
    void setLoggedIn(bool v);
    void setWaiting(bool v);
    void saveCredentials();
    void clearCredentials();

    ApiClient *m_api;
    QSettings m_settings;
    QTimer m_pollTimer;
    bool m_loggedIn = false;
    bool m_waitingForBrowser = false;
    bool m_restoringSession = false;
    QString m_serverUrl;
    QString m_userId;
    QString m_displayName;
    QString m_error;
    QString m_status;
    QString m_user;
    QString m_password;
    QString m_ncVersion;
    QString m_talkVersion;
    QString m_signalingMode;
    talq::ServerCapabilities m_capabilities;
    // `spreed.config` verbatim. Replaced wholesale on each capabilities fetch
    // and cleared on logout, for the same reason ServerCapabilities refuses to
    // merge: one process can sign out of a Talk 24 server and into an older
    // one, and carrying the first server's config across would answer questions
    // about the second server with the first one's settings.
    QJsonObject m_serverConfig;
    // Bounded retry for the one-shot capabilities fetch. Without it a single
    // lost request at login disables every gated feature for the session.
    static constexpr int kMaxCapabilityRetries = 3;
    int m_capabilityRetries = 0;

    // Login Flow v2 poll state
    QString m_pollToken;
    QString m_pollEndpoint;
    int m_pollAttempts = 0;
    static constexpr int MAX_POLL_ATTEMPTS = 150; // 5 minutes at 2s intervals
};
