#include "core/AuthManager.h"
#include <QDesktopServices>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

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
    QString password = m_settings.value("auth/password").toString();

    if (server.isEmpty() || user.isEmpty() || password.isEmpty()) {
        qDebug() << "No saved credentials";
        m_restoringSession = true;
        emit restoringChanged();
        m_restoringSession = false;
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
    m_api->get("cloud/user", [this](bool ok, const QJsonObject &data, int) {
        m_restoringSession = false;
        emit restoringChanged();

        if (!ok) {
            setError("Authentication failed. Please log in again.");
            setStatus({});
            clearCredentials();
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
    m_api->get("cloud/capabilities", [this](bool ok, const QJsonObject &data, int) {
        if (!ok) return;

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

        // Thread support (Talk v22+)
        auto features = spreed["features"].toArray();
        m_hasThreads = false;
        for (const auto &f : features) {
            if (f.toString() == "threads") {
                m_hasThreads = true;
                break;
            }
        }

        qDebug() << "Server info: NC" << m_ncVersion << "Talk" << m_talkVersion
                 << "Signaling:" << m_signalingMode << "Threads:" << m_hasThreads;
        emit serverInfoChanged();
    });
}

void AuthManager::logout()
{
    cancelLogin();
    clearCredentials();
    m_api->cancelAll();
    m_api->setCredentials({}, {});
    setLoggedIn(false);
    m_userId.clear();
    m_displayName.clear();
    emit userInfoChanged();
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
    m_settings.setValue("auth/password", m_password);
}

void AuthManager::clearCredentials()
{
    m_settings.remove("auth/serverUrl");
    m_settings.remove("auth/user");
    m_settings.remove("auth/password");
}
