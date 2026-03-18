#pragma once

#include <QObject>
#include <QSettings>
#include <QTimer>
#include "core/ApiClient.h"

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

    // Login Flow v2 poll state
    QString m_pollToken;
    QString m_pollEndpoint;
    int m_pollAttempts = 0;
    static constexpr int MAX_POLL_ATTEMPTS = 150; // 5 minutes at 2s intervals
};
