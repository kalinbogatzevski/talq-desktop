#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QTimer>

class ApiClient;
class AuthManager;
class QColor;
class QJsonObject;

/**
 * Owns the current user's Nextcloud user-status (the user_status app), the
 * full protocol behind it, and the crash-stuck "in a call" recovery.
 *
 * The "📞 In a call / Busy" status is set and reverted server-side by Talk
 * as a side effect of call participation; the client never sets it. What
 * the client CAN do — and does here on every login — is call
 * `user_status/revert/call`, which restores the pre-call backup if a crash
 * left it stuck. Everything else (online/away/dnd/invisible, custom
 * message, predefined presets, clear-after) is a normal user_status write.
 */
class UserStatusManager : public QObject
{
    Q_OBJECT
public:
    enum class Status { Online, Away, Dnd, Invisible, Offline };

    struct Predefined {
        QString id;
        QString icon;
        QString message;
    };

    UserStatusManager(ApiClient *api, AuthManager *auth, QObject *parent = nullptr);

    Status  status() const { return m_status; }
    QString message() const { return m_message; }
    QString icon() const { return m_icon; }
    QString messageId() const { return m_messageId; }
    qint64  clearAt() const { return m_clearAt; }
    bool    isUserDefined() const { return m_userDefined; }
    const QVector<Predefined> &predefinedStatuses() const { return m_predefined; }

    // Shared with the contacts' presence dots so own/other dots match.
    static QColor colorFor(Status s);
    static QString label(Status s);
    static QString statusKey(Status s);          // "online"/"away"/...
    static Status  statusFromKey(const QString &k);

public slots:
    // Login/restore: load predefined list + current status, then revert a
    // crash-stuck 'call' status. Logout: reset + stop heartbeat.
    void onLoggedIn();
    void onLoggedOut();

    void setStatusType(Status s);
    void setPredefined(const QString &messageId, qint64 clearAt);
    void setCustom(const QString &icon, const QString &text, qint64 clearAt);
    void clearStatusMessage();

signals:
    void statusChanged();
    void predefinedLoaded();
    void error(const QString &msg);

private:
    void fetchPredefined();
    void fetchCurrent();
    void revertStuckCall();
    void applyFromJson(const QJsonObject &d);
    void takeSnapshot();
    void rollback();

    ApiClient   *m_api;
    AuthManager *m_auth;

    Status  m_status = Status::Offline;
    QString m_message;
    QString m_icon;
    QString m_messageId;
    qint64  m_clearAt = 0;
    bool    m_userDefined = false;
    QVector<Predefined> m_predefined;

    // Rollback snapshot for optimistic writes.
    Status  m_snapStatus = Status::Offline;
    QString m_snapMessage, m_snapIcon, m_snapMessageId;
    qint64  m_snapClearAt = 0;
    bool    m_snapUserDefined = false;

    // Keeps automatic presence alive; never overwrites a user-set state.
    QTimer m_heartbeat;
};
