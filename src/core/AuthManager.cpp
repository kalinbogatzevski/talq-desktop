#include "core/AuthManager.h"
#include <QDesktopServices>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#pragma comment(lib, "advapi32.lib")
#endif

// --- Secure credential storage via Windows Credential Manager ---

static const wchar_t *CRED_TARGET = L"TalQ/NextcloudAppPassword";

static void savePasswordSecure(const QString &password)
{
#ifdef Q_OS_WIN
    QByteArray utf8 = password.toUtf8();
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(CRED_TARGET);
    cred.CredentialBlobSize = static_cast<DWORD>(utf8.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(utf8.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    CredWriteW(&cred, 0);
#else
    Q_UNUSED(password)
#endif
}

static QString loadPasswordSecure()
{
#ifdef Q_OS_WIN
    PCREDENTIALW cred = nullptr;
    if (CredReadW(CRED_TARGET, CRED_TYPE_GENERIC, 0, &cred)) {
        QString pw = QString::fromUtf8(
            reinterpret_cast<const char *>(cred->CredentialBlob),
            static_cast<int>(cred->CredentialBlobSize));
        CredFree(cred);
        return pw;
    }
#endif
    return {};
}

static void deletePasswordSecure()
{
#ifdef Q_OS_WIN
    CredDeleteW(CRED_TARGET, CRED_TYPE_GENERIC, 0);
#endif
}

AuthManager::AuthManager(ApiClient *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_settings("TalkQt", "TalkQt")
{
    m_pollTimer.setInterval(2000);
    connect(&m_pollTimer, &QTimer::timeout, this, &AuthManager::pollForCredentials);
}

void AuthManager::tryRestore()
{
    QString server = m_settings.value("auth/serverUrl").toString();
    QString user = m_settings.value("auth/user").toString();

    // Load password from secure storage (Windows Credential Manager)
    QString password = loadPasswordSecure();

    // Migration: if password was in QSettings (old format), migrate to secure storage
    if (password.isEmpty()) {
        password = m_settings.value("auth/password").toString();
        if (!password.isEmpty()) {
            savePasswordSecure(password);
            m_settings.remove("auth/password");  // remove from plaintext registry
            qDebug() << "Migrated password from QSettings to Credential Manager";
        }
    }

    if (server.isEmpty() || user.isEmpty() || password.isEmpty()) {
        qDebug() << "No saved credentials";
        // MainWindow is shown only via switchToChat()/switchToLogin(), both
        // driven by these signals. Without an emit here the no-credentials
        // path (e.g. right after logout) leaves the window never shown — it
        // looks like the app started minimized/tray-only with no way to log
        // in. m_restoringSession is already false, so the restoringChanged
        // handler takes its `else` branch and shows the login page normally.
        emit restoringChanged();
        return;
    }

    m_serverUrl = server;
    m_user = user;
    m_password = password;
    m_api->setServerUrl(server);
    m_api->setCredentials(user, password);
    emit serverUrlChanged();

    // Show loading screen while verifying
    m_restoringSession = true;
    emit restoringChanged();
    fetchUserInfo();
}

void AuthManager::startLogin(const QString &serverUrl)
{
    setError({});
    setStatus("Connecting...");

    QString url = serverUrl.trimmed();
    while (url.endsWith('/'))
        url.chop(1);

    if (!url.startsWith("http://") && !url.startsWith("https://"))
        url.prepend("https://");

    if (url.startsWith("http://"))
        qWarning() << "AuthManager: WARNING — using unencrypted HTTP connection. Credentials will be sent in cleartext.";

    m_serverUrl = url;
    m_api->setServerUrl(url);
    emit serverUrlChanged();

    // Step 1: Verify the server has Talk installed
    m_api->get("cloud/capabilities", [this](bool ok, const QJsonObject &data, int) {
        if (!ok) {
            setError("Could not connect to server. Check the URL and try again.");
            setStatus({});
            return;
        }

        auto caps = data["capabilities"].toObject();
        if (!caps.contains("spreed")) {
            setError("This server does not have Nextcloud Talk installed.");
            setStatus({});
            return;
        }

        // Step 2: Initiate Login Flow v2
        initiateLoginFlow();
    });
}

void AuthManager::initiateLoginFlow()
{
    setStatus("Starting login flow...");

    // POST directly to the login/v2 endpoint (not OCS — raw Nextcloud endpoint)
    QNetworkRequest req(QUrl(m_serverUrl + "/index.php/login/v2"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    req.setHeader(QNetworkRequest::UserAgentHeader, "TalQ");

    // Use the internal NAM from ApiClient for consistency
    // But this endpoint is unauthenticated and non-OCS, so we use a local NAM
    auto *nam = new QNetworkAccessManager(this);
    auto *reply = nam->post(req, QByteArray());

    connect(reply, &QNetworkReply::finished, this, [this, reply, nam]() {
        reply->deleteLater();
        nam->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            setError("Failed to start login flow: " + reply->errorString());
            setStatus({});
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject root = doc.object();

        QString loginUrl = root["login"].toString();
        QJsonObject poll = root["poll"].toObject();
        m_pollToken = poll["token"].toString();
        m_pollEndpoint = poll["endpoint"].toString();

        if (loginUrl.isEmpty() || m_pollToken.isEmpty() || m_pollEndpoint.isEmpty()) {
            setError("Server returned invalid login flow response.");
            setStatus({});
            return;
        }

        // Step 3: Open login URL in system browser
        QDesktopServices::openUrl(QUrl(loginUrl));
        setStatus("Waiting for login in your browser...");
        setWaiting(true);

        // Step 4: Start polling for credentials
        m_pollAttempts = 0;
        m_pollTimer.start();
    });
}

void AuthManager::pollForCredentials()
{
    m_pollAttempts++;

    if (m_pollAttempts > MAX_POLL_ATTEMPTS) {
        m_pollTimer.stop();
        setWaiting(false);
        setError("Login timed out. Please try again.");
        setStatus({});
        return;
    }

    // POST to the poll endpoint with the token
    auto *nam = new QNetworkAccessManager(this);
    QNetworkRequest req{QUrl(m_pollEndpoint)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QByteArray body = "token=" + QUrl::toPercentEncoding(m_pollToken);
    auto *reply = nam->post(req, body);

    connect(reply, &QNetworkReply::finished, this, [this, reply, nam]() {
        reply->deleteLater();
        nam->deleteLater();

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (status == 404) {
            // Not yet authenticated — keep polling
            return;
        }

        if (reply->error() != QNetworkReply::NoError && status != 200) {
            // Transient error — keep trying
            return;
        }

        // Success — we have credentials
        m_pollTimer.stop();

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject root = doc.object();

        QString server = root["server"].toString();
        QString loginName = root["loginName"].toString();
        QString appPassword = root["appPassword"].toString();

        if (server.isEmpty() || loginName.isEmpty() || appPassword.isEmpty()) {
            setWaiting(false);
            setError("Invalid credentials received from server.");
            setStatus({});
            return;
        }

        // Store and apply credentials
        m_serverUrl = server;
        m_user = loginName;
        m_password = appPassword;
        m_api->setServerUrl(server);
        m_api->setCredentials(loginName, appPassword);
        emit serverUrlChanged();

        saveCredentials();
        setStatus("Login successful! Loading...");
        setWaiting(false);
        fetchUserInfo();
    });
}

void AuthManager::cancelLogin()
{
    m_pollTimer.stop();
    m_pollToken.clear();
    m_pollEndpoint.clear();
    setWaiting(false);
    setStatus({});
    setError({});
}

void AuthManager::fetchUserInfo()
{
    m_api->get("cloud/user", [this](bool ok, const QJsonObject &data, int statusCode) {
        m_restoringSession = false;
        emit restoringChanged();

        if (!ok) {
            if (statusCode == 0) {
                // Network error (DNS, timeout, no connection)
                setError("Cannot connect to server. Check your network.");
            } else if (statusCode == 401 || statusCode == 403) {
                setError("Authentication failed. Please log in again.");
                clearCredentials();
            } else {
                setError("Server error (" + QString::number(statusCode) + "). Try again later.");
            }
            setStatus({});
            setLoggedIn(false);
            return;
        }

        m_userId = data["id"].toString();
        m_displayName = data["displayname"].toString();
        if (m_displayName.isEmpty())
            m_displayName = m_userId;

        emit userInfoChanged();
        setLoggedIn(true);
        setStatus({});
        qDebug() << "Logged in as" << m_displayName << "(" << m_userId << ")";

        fetchServerInfo();
    });
}

void AuthManager::fetchServerInfo()
{
    m_api->get("cloud/capabilities", [this](bool ok, const QJsonObject &data, int status) {
        if (!ok) {
            // Capabilities are fetched exactly once per session and every
            // gated feature reads them, so losing this one request used to
            // mean silently running the whole session as if the server were
            // ancient — tags and presets simply missing, on one machine only,
            // with nothing in the log to explain it. Retry a few times with
            // backoff before giving up, and say so out loud when we do.
            if (m_capabilityRetries < kMaxCapabilityRetries) {
                const int delayMs = 2000 * (1 << m_capabilityRetries);  // 2s, 4s, 8s
                ++m_capabilityRetries;
                qWarning() << "capabilities: fetch failed (status" << status
                           << ") — retry" << m_capabilityRetries << "of"
                           << kMaxCapabilityRetries << "in" << delayMs << "ms";
                QTimer::singleShot(delayMs, this, [this]() {
                    if (m_loggedIn) fetchServerInfo();
                });
            } else {
                qWarning() << "capabilities: giving up after"
                           << kMaxCapabilityRetries
                           << "attempts — server features stay DISABLED for "
                              "this session (tags, presets and any other "
                              "capability-gated feature will not appear)";
            }
            return;
        }
        m_capabilityRetries = 0;

        // Nextcloud version
        auto version = data["version"].toObject();
        m_ncVersion = version["string"].toString();
        if (m_ncVersion.isEmpty()) {
            int major = version["major"].toInt();
            int minor = version["minor"].toInt();
            int micro = version["micro"].toInt();
            m_ncVersion = QString("%1.%2.%3").arg(major).arg(minor).arg(micro);
        }

        // Talk capabilities
        auto caps = data["capabilities"].toObject();
        auto spreed = caps["spreed"].toObject();

        // Talk version from features
        auto talkVersion = spreed["version"].toString();
        if (!talkVersion.isEmpty())
            m_talkVersion = talkVersion;

        // Signaling mode
        auto config = spreed["config"].toObject();
        auto signaling = config["signaling"].toObject();
        auto signalingMode = signaling["signalingMode"].toString();
        if (!signalingMode.isEmpty()) {
            if (signalingMode == "internal")
                m_signalingMode = "Internal";
            else if (signalingMode == "external")
                m_signalingMode = "High Performance Backend";
            else if (signalingMode == "clustered")
                m_signalingMode = "Clustered HPB";
            else
                m_signalingMode = signalingMode;
        } else {
            m_signalingMode = "Internal";
        }

        // Feature capabilities. Keep the WHOLE list: before 0.65.0 this loop
        // extracted the single literal "threads" and dropped everything else,
        // which left no way to gate anything version-dependent. setFeatures()
        // replaces the set wholesale rather than merging, so logging out of a
        // Talk 24 server and into an older one in the same process cannot carry
        // the newer server's flags across.
        auto features = spreed["features"].toArray();
        std::vector<std::string> featureList;
        featureList.reserve(features.size());
        for (const auto &f : features) {
            const QString name = f.toString();
            if (!name.isEmpty())
                featureList.push_back(name.toStdString());
        }
        m_capabilities.setFeatures(featureList);

        qDebug() << "Server info: NC" << m_ncVersion << "Talk" << m_talkVersion
                 << "Signaling:" << m_signalingMode
                 << "| features:" << int(m_capabilities.size())
                 << "threads:" << hasThreadsSupport()
                 << "tags:" << supportsConversationTags()
                 << "presets:" << supportsConversationPresets();
        emit serverInfoChanged();
    });
}

void AuthManager::logout()
{
    cancelLogin();
    clearCredentials();
    m_api->cancelAll();
    m_api->setCredentials({}, {});
    // Clear credentials from memory (zero before releasing)
    m_password.fill(QChar(0)); m_password.clear();
    m_user.fill(QChar(0)); m_user.clear();
    m_serverUrl.clear();
    setLoggedIn(false);
    m_userId.clear();
    m_displayName.clear();
    // Drop the server's feature set with the session. Leaving it behind would
    // let the PREVIOUS server's capabilities gate decisions made against the
    // next one — the sign-out-of-one-server, sign-in-to-an-older-one case.
    m_capabilities.reset();
    m_capabilityRetries = 0;
    emit userInfoChanged();
    emit serverInfoChanged();
}

void AuthManager::setError(const QString &msg)
{
    if (m_error != msg) {
        m_error = msg;
        emit errorChanged();
    }
}

void AuthManager::setStatus(const QString &msg)
{
    if (m_status != msg) {
        m_status = msg;
        emit statusChanged();
    }
}

void AuthManager::setLoggedIn(bool v)
{
    if (m_loggedIn != v) {
        m_loggedIn = v;
        emit loggedInChanged();
    }
}

void AuthManager::setWaiting(bool v)
{
    if (m_waitingForBrowser != v) {
        m_waitingForBrowser = v;
        emit waitingChanged();
    }
}

void AuthManager::saveCredentials()
{
    m_settings.setValue("auth/serverUrl", m_serverUrl);
    m_settings.setValue("auth/user", m_user);
    // Password goes to Windows Credential Manager, NOT QSettings
    savePasswordSecure(m_password);
    m_settings.remove("auth/password");  // ensure no plaintext copy
}

void AuthManager::clearCredentials()
{
    m_settings.remove("auth/serverUrl");
    m_settings.remove("auth/user");
    m_settings.remove("auth/password");  // clean up any legacy
    deletePasswordSecure();
}
